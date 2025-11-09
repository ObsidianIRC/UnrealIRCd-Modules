/*
 * ObbyScript module for UnrealIRCd
 * (C) 2025 Valerie Pond <v.a.pond@outlook.com>
 * License: GPLv3
 * 
 * This module implements a simple scripting language "ObbyScript" for UnrealIRCd.
 * 
 */

/*** <<<MODULE MANAGER START>>>
module
{
        documentation "https://github.com/ValwareIRC/valware-unrealircd-mods/blob/main/obbyscript/README.md";
        troubleshooting "In case of problems, documentation or e-mail me at v.a.pond@outlook.com";
        min-unrealircd-version "6.*";
        max-unrealircd-version "6.*";
        post-install-text {
                "The module is installed. Now all you need to do is add a loadmodule line:";
                "loadmodule \"third/obbyscript\";";
                "And configure script files in your unrealircd.conf:";
                "scripts {";
                "    \"/path/to/script.us\";";
                "}";
                "Then /REHASH the IRCd.";
        }
}
*** <<<MODULE MANAGER END>>>
*/

#include "unrealircd.h"

ModuleHeader MOD_HEADER = {
	"third/obbyscript",
	"1.0",
	"ObbyScript scripting language",
	"Valware",
	"unrealircd-6",
};

/* Configuration */
#define MYCONF "scripts"

/* Event types - significantly expanded */
typedef enum {
	US_EVENT_START,
	US_EVENT_CONNECT,
	US_EVENT_QUIT,
	US_EVENT_CAN_JOIN,
	US_EVENT_JOIN,
	US_EVENT_PART,
	US_EVENT_KICK,
	US_EVENT_NICK,
	US_EVENT_PRIVMSG,
	US_EVENT_NOTICE,
	US_EVENT_TOPIC,
	US_EVENT_MODE,
	US_EVENT_INVITE,
	US_EVENT_KNOCK,
	US_EVENT_AWAY,
	US_EVENT_OPER,
	US_EVENT_KILL,
	US_EVENT_UMODE_CHANGE,
	US_EVENT_CHANMODE,
	US_EVENT_CHANNEL_CREATE,
	US_EVENT_CHANNEL_DESTROY,
	US_EVENT_WHOIS,
	US_EVENT_REHASH,
	US_EVENT_ACCOUNT_LOGIN,
	US_EVENT_PRE_COMMAND,
	US_EVENT_POST_COMMAND,
	US_EVENT_TKL_ADD,
	US_EVENT_TKL_DEL,
	US_EVENT_SPAMFILTER,
	US_EVENT_COMMAND_OVERRIDE,  /* on COMMAND:EXAMPLECMD */
	US_EVENT_COMMAND_NEW,       /* new COMMAND:EXAMPLECMD */
	US_EVENT_MAX
} ObbyScriptEventType;

/* Action types - expanded to use do_cmd */
typedef enum {
	US_ACTION_COMMAND,    /* Generic command execution via do_cmd */
	US_ACTION_IF,
	US_ACTION_ELSE,       /* Else statement */
	US_ACTION_WHILE,      /* While loop */
	US_ACTION_FOR,        /* For loop */
	US_ACTION_SENDNOTICE, /* Legacy - will be converted to COMMAND */
	US_ACTION_RETURN,     /* Return value (for CAN_JOIN events) */
	US_ACTION_BREAK,      /* Break out of loop */
	US_ACTION_CONTINUE,   /* Continue to next iteration */
	US_ACTION_VAR,        /* Variable declaration/assignment */
	US_ACTION_ARITHMETIC, /* Arithmetic operations */
	US_ACTION_ISUPPORT,   /* Add ISUPPORT token */
	US_ACTION_CAP,        /* Add CAP capability */
	US_ACTION_FUNCTION_DEF, /* Function definition */
	US_ACTION_FUNCTION_CALL, /* Function call */
	US_ACTION_MAX
} ObbyScriptActionType;

/* Variable types */
typedef enum {
	US_VAR_CLIENT,
	US_VAR_CHANNEL,
	US_VAR_STRING,
	US_VAR_ARRAY,
	US_VAR_MAX
} ObbyScriptVarType;

/* Array element structure */
typedef struct ObbyScriptArrayElement {
	ObbyScriptVarType type;
	char *string_value;
	void *object_ptr;  /* For client/channel objects */
	struct ObbyScriptArrayElement *next;
} ObbyScriptArrayElement;

/* Array structure */
typedef struct ObbyScriptArray {
	ObbyScriptArrayElement **elements;  /* Array of element pointers for fast indexed access */
	int size;                              /* Current number of elements */
	int capacity;                          /* Allocated capacity */
} ObbyScriptArray;

/* Boolean expression types for compound conditions */
typedef enum {
	US_EXPR_SIMPLE,      /* Simple comparison: variable op value */
	US_EXPR_AND,         /* Logical AND: left && right */
	US_EXPR_OR,          /* Logical OR: left || right */
	US_EXPR_PARENTHESES  /* Grouped expression: (expr) */
} ObbyScriptExprType;

/* Forward declaration */
struct ObbyScriptBoolExpr;

/* Script condition - now part of boolean expressions */
typedef struct ObbyScriptCondition {
	char *variable;     /* e.g., "$client.umodes" */
	char *operator;     /* e.g., "has", "!has", "==", "!=", "in", "insg", "hascap", "ischanop", "isvoice", "ishalfop", "isadmin", "isowner" */
	char *value;        /* e.g., "UMODE_OPER" */
	struct ObbyScriptCondition *next;
} ObbyScriptCondition;

/* Boolean expression tree for compound conditions */
typedef struct ObbyScriptBoolExpr {
	ObbyScriptExprType type;
	union {
		ObbyScriptCondition *simple;  /* For US_EXPR_SIMPLE */
		struct {
			struct ObbyScriptBoolExpr *left;
			struct ObbyScriptBoolExpr *right;
		} compound;  /* For US_EXPR_AND, US_EXPR_OR */
		struct ObbyScriptBoolExpr *grouped;  /* For US_EXPR_PARENTHESES */
	} data;
} ObbyScriptBoolExpr;

/* Script action */
typedef struct ObbyScriptAction {
	ObbyScriptActionType type;
	char *function;     /* Function name like "sendnotice", "kick" */
	char **args;        /* Array of arguments */
	int argc;           /* Number of arguments */
	ObbyScriptCondition *condition; /* Legacy: Simple condition (for backward compatibility) */
	ObbyScriptBoolExpr *bool_expr;  /* New: Boolean expression tree for compound conditions */
	struct ObbyScriptAction *nested_actions; /* Actions inside if blocks */
	struct ObbyScriptAction *else_actions;   /* Actions inside else blocks */
	/* Loop-specific fields */
	char *loop_var;     /* Loop variable name (for 'for' loops) */
	int loop_start;     /* Starting value for range-based loops */
	int loop_end;       /* Ending value for range-based loops */
	int loop_step;      /* Step value (default 1) */
	char *loop_init;    /* Initialization expression (C-style for loops) */
	char *loop_increment; /* Increment expression (C-style for loops) */
	struct ObbyScriptAction *next;
} ObbyScriptAction;

/* Script rule */
typedef struct ObbyScriptRule {
	ObbyScriptEventType event;
	char *target;       /* Channel/user target pattern like "*", "#opers" */
	ObbyScriptAction *actions;
	struct ObbyScriptRule *next;
} ObbyScriptRule;

/* Script file */
typedef struct ObbyScriptFile {
	char *filename;
	ObbyScriptRule *rules;
	struct ObbyScriptFile *next;
} ObbyScriptFile;

/* Channel snapshot to avoid use-after-free issues */
typedef struct ObbyScriptChannelSnapshot {
	char *name;
	char *topic;
	int user_count;
} ObbyScriptChannelSnapshot;

/* Variable structure */
typedef struct ObbyScriptVariable {
	char *name;         /* Variable name (without %) */
	char *value;        /* Variable value (for strings) */
	ObbyScriptVarType type; /* Variable type (string, client, channel, array) */
	void *object_ptr;   /* Pointer to client/channel/server object */
	struct ObbyScriptArray *array_ptr; /* Pointer to array data */
	int is_const;       /* 1 if const var, 0 if regular var */
	struct ObbyScriptVariable *next;
} ObbyScriptVariable;

/* Script-level variable scope */
typedef struct ObbyScriptScope {
	ObbyScriptVariable *variables;
	struct ObbyScriptScope *parent;  /* For nested scopes later */
} ObbyScriptScope;

/* Deferred action structure to avoid executing destructive commands during hook processing */
typedef struct ObbyScriptDeferredAction {
	char *command;
	char **args;
	int argc;
	char *client_name;  /* Store client name to look up later */
	char *channel_name; /* Store channel name to look up later */
	struct ObbyScriptDeferredAction *next;
} ObbyScriptDeferredAction;

/* CAP capability structure for registration */
typedef struct ObbyScriptCapability {
	char *name;
	struct ObbyScriptCapability *next;
} ObbyScriptCapability;

/* Function definition structure */
typedef struct ObbyScriptFunction {
	char *name;          /* Function name (without $) */
	char **parameters;   /* Array of parameter names (without $) */
	int param_count;     /* Number of parameters */
	struct ObbyScriptAction *body; /* Function body actions */
	struct ObbyScriptFunction *next;
} ObbyScriptFunction;

/* Global variables */
static ObbyScriptFile *script_files = NULL;
static ObbyScriptDeferredAction *deferred_actions = NULL;
static int executing_deferred_actions = 0;  /* Prevent recursive execution */
static ObbyScriptScope *global_scope = NULL;  /* Global variable scope */
static ObbyScriptCapability *pending_caps = NULL;  /* CAP capabilities to register */
static Module *obbyscript_module_handle = NULL;  /* Module handle for CAP registration */
static ObbyScriptFunction *global_functions = NULL;  /* Global function definitions */
static int in_join_context = 0;  /* Track when we're executing within a JOIN hook to handle bouncedtimes properly */
static int should_break = 0;  /* Global flag for break statement in loops */
static int should_continue = 0;  /* Global flag for continue statement in loops */

/* Command tracking structures */
typedef struct ObbyScriptCommand {
	char *command;              /* Command name */
	Command *cmd_ptr;           /* CommandAdd() result */
	CommandOverride *ovr_ptr;   /* CommandOverrideAdd() result */
	ObbyScriptRule *rule;     /* Script rule that defines this command */
	struct ObbyScriptCommand *next;
} ObbyScriptCommand;

static ObbyScriptCommand *registered_commands = NULL;

/* Command execution context (for parameter substitution) */
static int current_command_parc = 0;
static const char **current_command_parv = NULL;


/* Function prototypes */
int obbyscript_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int obbyscript_configrun(ConfigFile *cf, ConfigEntry *ce, int type);
ObbyScriptFile *load_script_file(const char *filename);
void free_script_file(ObbyScriptFile *file);
void free_script_rule(ObbyScriptRule *rule);
void free_script_action(ObbyScriptAction *action);
void free_script_condition(ObbyScriptCondition *condition);
void free_bool_expr(ObbyScriptBoolExpr *expr);
ObbyScriptBoolExpr *parse_bool_expression(const char *expr_str);
ObbyScriptCondition *parse_simple_condition(const char *cond_str);
int evaluate_bool_expr(ObbyScriptBoolExpr *expr, Client *client, Channel *channel);
ObbyScriptRule *parse_script_content(const char *content);
ObbyScriptEventType parse_event_type(const char *event_str);
ObbyScriptAction *parse_action_block(const char *content);
char *substitute_variables(const char *input, Client *client, Channel *channel);
void add_deferred_action(const char *command, const char **args, int argc, Client *client, Channel *channel);
void execute_deferred_actions();
void free_deferred_actions(void);
int is_destructive_command(const char *command);
void execute_script_action(ObbyScriptAction *action, Client *client, Channel *channel);
void execute_script_action_with_params(ObbyScriptAction *action, Client *client, Channel *channel, int parc, const char *parv[]);
int evaluate_condition(ObbyScriptCondition *condition, Client *client, Channel *channel);
char *obbyscript_replace_string(const char *haystack, const char *needle, const char *replacement);
void execute_scripts_for_event(ObbyScriptEventType event, Client *client, Channel *channel, const char *extra_data);

/* Variable management functions */
ObbyScriptScope *create_scope(ObbyScriptScope *parent);
void free_scope(ObbyScriptScope *scope);
void set_variable(ObbyScriptScope *scope, const char *name, const char *value, int is_const);
void set_variable_object(ObbyScriptScope *scope, const char *name, void *object_ptr, ObbyScriptVarType type, int is_const);
void set_variable_array(ObbyScriptScope *scope, const char *name, ObbyScriptArray *array, int is_const);
char *get_variable(ObbyScriptScope *scope, const char *name);
ObbyScriptVariable *get_variable_object(ObbyScriptScope *scope, const char *name);
ObbyScriptVariable *find_variable(ObbyScriptScope *scope, const char *name);
void init_global_scope(void);
void execute_start_events(void);

/* Array management functions */
ObbyScriptArray *create_array(int initial_capacity);
void free_array(ObbyScriptArray *array);
void array_push_string(ObbyScriptArray *array, const char *value);
void array_push_object(ObbyScriptArray *array, void *object_ptr, ObbyScriptVarType type);
char *array_get_string(ObbyScriptArray *array, int index);
void *array_get_object(ObbyScriptArray *array, int index, ObbyScriptVarType *out_type);
void array_set_string(ObbyScriptArray *array, int index, const char *value);
void array_set_object(ObbyScriptArray *array, int index, void *object_ptr, ObbyScriptVarType type);
ObbyScriptArray *parse_array_literal(const char *array_str, Client *client, Channel *channel);
ObbyScriptArray *get_client_channels(Client *client);

/* Arithmetic functions */
int evaluate_arithmetic(const char *expression, Client *client, Channel *channel);
int is_arithmetic_operation(const char *line);

/* CAP capability management functions */
void add_pending_cap(const char *cap_name);
void register_pending_caps(void);
void free_pending_caps(void);

/* Function management functions */
void add_function(const char *name, char **parameters, int param_count, ObbyScriptAction *body);
ObbyScriptFunction *find_function(const char *name);
void free_function(ObbyScriptFunction *func);
void free_all_functions(void);
int execute_function(const char *name, char **args, int arg_count, Client *client, Channel *channel, char **return_value);
ObbyScriptVariable *execute_function_with_return(const char *name, char **args, int arg_count, Client *client, Channel *channel);
int execute_function_with_objects(const char *name, char **args, ObbyScriptVariable **object_args, int arg_count, Client *client, Channel *channel, char **return_value);
int is_function_call(const char *line);
char *evaluate_condition_value(const char *condition, Client *client, Channel *channel);
int is_falsy_value(const char *value);

/* Built-in wrapper functions */
ObbyScriptVariable *builtin_find_client(char **args, int arg_count, Client *client, Channel *channel);
ObbyScriptVariable *builtin_find_server(char **args, int arg_count, Client *client, Channel *channel);
ObbyScriptVariable *builtin_find_channel(char **args, int arg_count, Client *client, Channel *channel);

/* Command management functions */
void register_script_commands(void);
void register_commands_for_file(ObbyScriptFile *file);
void unregister_script_commands(void);
CMD_FUNC(obbyscript_command_handler);
CMD_OVERRIDE_FUNC(obbyscript_command_override_handler);
char *substitute_command_parameters(const char *text, int parc, const char *parv[], Client *client, Channel *channel);
ObbyScriptVariable *obbyscript_find_client(char **args, int arg_count);
ObbyScriptVariable *obbyscript_find_server(char **args, int arg_count);
ObbyScriptVariable *obbyscript_find_channel(char **args, int arg_count);
int is_builtin_function(const char *name);
ObbyScriptVariable *execute_builtin_function(const char *name, char **args, int arg_count);

/* Hook functions - significantly expanded */
int obbyscript_local_connect(Client *client);
int obbyscript_remote_connect(Client *client);
int obbyscript_can_join(Client *client, Channel *channel, const char *key, char **errmsg);
int obbyscript_local_join(Client *client, Channel *channel, MessageTag *mtags);
int obbyscript_remote_join(Client *client, Channel *channel, MessageTag *mtags);
int obbyscript_local_part(Client *client, Channel *channel, MessageTag *mtags, const char *comment);
int obbyscript_remote_part(Client *client, Channel *channel, MessageTag *mtags, const char *comment);
int obbyscript_local_quit(Client *client, MessageTag *mtags, const char *comment);
int obbyscript_remote_quit(Client *client, MessageTag *mtags, const char *comment);
int obbyscript_local_kick(Client *client, Client *victim, Channel *channel, MessageTag *mtags, const char *comment);
int obbyscript_remote_kick(Client *client, Client *victim, Channel *channel, MessageTag *mtags, const char *comment);
int obbyscript_local_nickchange(Client *client, MessageTag *mtags, const char *oldnick);
int obbyscript_remote_nickchange(Client *client, MessageTag *mtags, const char *oldnick);
int obbyscript_chanmsg(Client *client, Channel *channel, int sendflags, const char *member_modes, const char *target, MessageTag *mtags, const char *text, SendType sendtype);
int obbyscript_usermsg(Client *client, Client *to, MessageTag *mtags, const char *text, SendType sendtype);
int obbyscript_topic(Client *client, Channel *channel, MessageTag *mtags, const char *topic);
int obbyscript_local_chanmode(Client *client, Channel *channel, MessageTag *mtags, const char *modebuf, const char *parabuf, time_t sendts, int samode, int *destroy_channel);
int obbyscript_remote_chanmode(Client *client, Channel *channel, MessageTag *mtags, const char *modebuf, const char *parabuf, time_t sendts, int samode, int *destroy_channel);
int obbyscript_invite(Client *client, Client *target, Channel *channel, MessageTag *mtags);
int obbyscript_knock(Client *client, Channel *channel, MessageTag *mtags, const char *comment);
int obbyscript_away(Client *client, MessageTag *mtags, const char *reason, int returning);
int obbyscript_local_oper(Client *client, int add, const char *oper_block, const char *operclass);
int obbyscript_local_kill(Client *client, Client *victim, const char *reason);
int obbyscript_umode_change(Client *client, long setflags, long newflags);
int obbyscript_channel_create(Channel *channel);
int obbyscript_channel_destroy(Channel *channel, int *should_destroy);
int obbyscript_whois(Client *client, Client *target, NameValuePrioList **list);
int obbyscript_rehash(void);
int obbyscript_account_login(Client *client, MessageTag *mtags);
int obbyscript_pre_command(Client *from, MessageTag *mtags, const char *buf);
int obbyscript_post_command(Client *from, MessageTag *mtags, const char *buf);
int obbyscript_tkl_add(Client *client, TKL *tkl);
int obbyscript_tkl_del(Client *client, TKL *tkl);

/* Timer event for executing deferred actions */
EVENT(obbyscript_execute_deferred_timer)
{
	/* Execute deferred actions if any exist */
	if (deferred_actions && !executing_deferred_actions)
	{
		execute_deferred_actions();
	}
}

MOD_TEST()
{
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, obbyscript_configtest);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	MARK_AS_GLOBAL_MODULE(modinfo);
	
	/* Store module handle for later use */
	obbyscript_module_handle = modinfo->handle;
	
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, obbyscript_configrun);
	
	/* Connection hooks */
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_CONNECT, 0, obbyscript_local_connect);
	HookAdd(modinfo->handle, HOOKTYPE_REMOTE_CONNECT, 0, obbyscript_remote_connect);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_QUIT, 0, obbyscript_local_quit);
	HookAdd(modinfo->handle, HOOKTYPE_REMOTE_QUIT, 0, obbyscript_remote_quit);
	
	/* Channel hooks */
	HookAdd(modinfo->handle, HOOKTYPE_CAN_JOIN, 0, obbyscript_can_join);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_JOIN, 0, obbyscript_local_join);
	HookAdd(modinfo->handle, HOOKTYPE_REMOTE_JOIN, 0, obbyscript_remote_join);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_PART, 0, obbyscript_local_part);
	HookAdd(modinfo->handle, HOOKTYPE_REMOTE_PART, 0, obbyscript_remote_part);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_KICK, 0, obbyscript_local_kick);
	HookAdd(modinfo->handle, HOOKTYPE_REMOTE_KICK, 0, obbyscript_remote_kick);
	HookAdd(modinfo->handle, HOOKTYPE_CHANNEL_CREATE, 0, obbyscript_channel_create);
	HookAdd(modinfo->handle, HOOKTYPE_CHANNEL_DESTROY, 0, obbyscript_channel_destroy);
	
	/* Nick hooks */
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_NICKCHANGE, 0, obbyscript_local_nickchange);
	HookAdd(modinfo->handle, HOOKTYPE_REMOTE_NICKCHANGE, 0, obbyscript_remote_nickchange);
	
	/* Message hooks */
	HookAdd(modinfo->handle, HOOKTYPE_CHANMSG, 0, obbyscript_chanmsg);
	HookAdd(modinfo->handle, HOOKTYPE_USERMSG, 0, obbyscript_usermsg);
	
	/* Channel management hooks */
	HookAdd(modinfo->handle, HOOKTYPE_TOPIC, 0, obbyscript_topic);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_CHANMODE, 0, obbyscript_local_chanmode);
	HookAdd(modinfo->handle, HOOKTYPE_REMOTE_CHANMODE, 0, obbyscript_remote_chanmode);
	HookAdd(modinfo->handle, HOOKTYPE_INVITE, 0, obbyscript_invite);
	HookAdd(modinfo->handle, HOOKTYPE_KNOCK, 0, obbyscript_knock);
	
	/* User status hooks */
	HookAdd(modinfo->handle, HOOKTYPE_AWAY, 0, obbyscript_away);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_OPER, 0, obbyscript_local_oper);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_KILL, 0, obbyscript_local_kill);
	HookAdd(modinfo->handle, HOOKTYPE_UMODE_CHANGE, 0, obbyscript_umode_change);
	
	/* Administrative hooks */
	HookAdd(modinfo->handle, HOOKTYPE_WHOIS, 0, obbyscript_whois);
	HookAdd(modinfo->handle, HOOKTYPE_REHASH, 0, obbyscript_rehash);
	HookAdd(modinfo->handle, HOOKTYPE_ACCOUNT_LOGIN, 0, obbyscript_account_login);
	
	/* Command hooks */
	HookAdd(modinfo->handle, HOOKTYPE_PRE_COMMAND, 0, obbyscript_pre_command);
	HookAdd(modinfo->handle, HOOKTYPE_POST_COMMAND, 0, obbyscript_post_command);
	
	/* TKL (ban) hooks */
	HookAdd(modinfo->handle, HOOKTYPE_TKL_ADD, 0, obbyscript_tkl_add);
	HookAdd(modinfo->handle, HOOKTYPE_TKL_DEL, 0, obbyscript_tkl_del);
	
	/* Initialize global variable scope */
	init_global_scope();
	
	/* Add timer for deferred action execution (every 10ms) */
	EventAdd(modinfo->handle, "obbyscript_deferred_timer", obbyscript_execute_deferred_timer, NULL, 10, 0);
	
	return MOD_SUCCESS;
}

MOD_LOAD()
{
	/* Commands are registered when script files are loaded */
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	ObbyScriptFile *file, *next_file;
	ObbyScriptCapability *cap, *next_cap;
	
	/* Unregister all script-defined commands */
	unregister_script_commands();
	
	/* Free all loaded scripts */
	for (file = script_files; file; file = next_file)
	{
		next_file = file->next;
		free_script_file(file);
	}
	script_files = NULL;
	
	/* Free global variable scope */
	if (global_scope)
	{
		free_scope(global_scope);
		global_scope = NULL;
	}
	
	/* Free all functions */
	free_all_functions();
	
	/* Free pending capabilities list */
	for (cap = pending_caps; cap; cap = next_cap)
	{
		next_cap = cap->next;
		if (cap->name)
			free(cap->name);
		free(cap);
	}
	pending_caps = NULL;
	
	return MOD_SUCCESS;
}

int obbyscript_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	ConfigEntry *cep;

	if (type != CONFIG_MAIN)
		return 0;

	if (!ce || !ce->name || strcmp(ce->name, MYCONF))
		return 0;

	for (cep = ce->items; cep; cep = cep->next)
	{
		if (!cep->name)
		{
			config_error("%s:%i: blank %s item", cep->file->filename, cep->line_number, MYCONF);
			errors++;
			continue;
		}

		/* Each item should be a script filename */
		if (access(cep->name, R_OK) != 0)
		{
			config_error("%s:%i: script file '%s' does not exist or is not readable", 
				cep->file->filename, cep->line_number, cep->name);
			errors++;
		}
	}

	*errs = errors;
	return errors ? -1 : 1;
}

int obbyscript_configrun(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep;
	ObbyScriptFile *file;

	if (type != CONFIG_MAIN)
		return 0;

	if (!ce || !ce->name || strcmp(ce->name, MYCONF))
		return 0;

	/* Free existing scripts */
	while (script_files)
	{
		file = script_files;
		script_files = script_files->next;
		free_script_file(file);
	}

	/* Load new scripts */
	for (cep = ce->items; cep; cep = cep->next)
	{
		if (cep->name)
		{
			file = load_script_file(cep->name);
			if (file)
			{
				file->next = script_files;
				script_files = file;
				unreal_log(ULOG_DEBUG, "obbyscript", "SCRIPT_LOADED", NULL, 
					"Loaded ObbyScript file: $file", 
					log_data_string("file", cep->name));
			}
		}
	}
	
	/* Execute START events for all loaded scripts */
	execute_start_events();
	
	/* Register any CAP capabilities that were requested during START events */
	register_pending_caps();

	return 1;
}

ObbyScriptFile *load_script_file(const char *filename)
{
	FILE *file;
	char *content = NULL;
	long file_size;
	ObbyScriptFile *script_file;

	file = fopen(filename, "r");
	if (!file)
	{
		unreal_log(ULOG_ERROR, "obbyscript", "SCRIPT_LOAD_ERROR", NULL,
			"Cannot open script file: $file", 
			log_data_string("file", filename));
		return NULL;
	}

	/* Get file size */
	fseek(file, 0, SEEK_END);
	file_size = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (file_size > 1024 * 1024) /* 1MB limit */
	{
		unreal_log(ULOG_ERROR, "obbyscript", "SCRIPT_TOO_LARGE", NULL,
			"Script file too large: $file", 
			log_data_string("file", filename));
		fclose(file);
		return NULL;
	}

	/* Read content */
	content = safe_alloc(file_size + 1);
	if (fread(content, 1, file_size, file) != file_size)
	{
		unreal_log(ULOG_ERROR, "obbyscript", "SCRIPT_READ_ERROR", NULL,
			"Error reading script file: $file", 
			log_data_string("file", filename));
		safe_free(content);
		fclose(file);
		return NULL;
	}
	content[file_size] = '\0';
	fclose(file);

	/* Create script file structure */
	script_file = safe_alloc(sizeof(ObbyScriptFile));
	safe_strdup(script_file->filename, filename);
	script_file->rules = parse_script_content(content);

	safe_free(content);

	if (!script_file->rules)
	{
		unreal_log(ULOG_ERROR, "obbyscript", "SCRIPT_PARSE_ERROR", NULL,
			"Failed to parse script file: $file", 
			log_data_string("file", filename));
		free_script_file(script_file);
		return NULL;
	}

	/* Register commands for this script file */
	register_commands_for_file(script_file);

	return script_file;
}

void free_script_file(ObbyScriptFile *file)
{
	ObbyScriptRule *rule, *next_rule;

	if (!file)
		return;

	safe_free(file->filename);
	
	for (rule = file->rules; rule; rule = next_rule)
	{
		next_rule = rule->next;
		free_script_rule(rule);
	}

	safe_free(file);
}

void free_script_rule(ObbyScriptRule *rule)
{
	ObbyScriptAction *action, *next_action;

	if (!rule)
		return;

	safe_free(rule->target);
	
	for (action = rule->actions; action; action = next_action)
	{
		next_action = action->next;
		free_script_action(action);
	}

	safe_free(rule);
}

void free_script_action(ObbyScriptAction *action)
{
	int i;

	if (!action)
		return;

	safe_free(action->function);
	
	if (action->args)
	{
		for (i = 0; i < action->argc; i++)
			safe_free(action->args[i]);
		safe_free(action->args);
	}

	if (action->condition)
		free_script_condition(action->condition);

	if (action->bool_expr)
		free_bool_expr(action->bool_expr);

	if (action->nested_actions)
		free_script_action(action->nested_actions);

	if (action->else_actions)
		free_script_action(action->else_actions);

	/* Free loop-specific fields */
	safe_free(action->loop_var);
	safe_free(action->loop_init);
	safe_free(action->loop_increment);

	safe_free(action);
}

void free_bool_expr(ObbyScriptBoolExpr *expr)
{
	if (!expr)
		return;

	switch (expr->type)
	{
		case US_EXPR_SIMPLE:
			if (expr->data.simple)
				free_script_condition(expr->data.simple);
			break;
		case US_EXPR_AND:
		case US_EXPR_OR:
			if (expr->data.compound.left)
				free_bool_expr(expr->data.compound.left);
			if (expr->data.compound.right)
				free_bool_expr(expr->data.compound.right);
			break;
		case US_EXPR_PARENTHESES:
			if (expr->data.grouped)
				free_bool_expr(expr->data.grouped);
			break;
	}
	safe_free(expr);
}

void free_script_condition(ObbyScriptCondition *condition)
{
	ObbyScriptCondition *next;

	while (condition)
	{
		next = condition->next;
		safe_free(condition->variable);
		safe_free(condition->operator);
		safe_free(condition->value);
		safe_free(condition);
		condition = next;
	}
}

/* Helper: Find the position of an operator at the top level (not inside parentheses) */
static char *find_top_level_operator(const char *str, const char *op)
{
	int paren_depth = 0;
	int op_len = strlen(op);
	const char *p = str;
	
	while (*p)
	{
		if (*p == '(')
			paren_depth++;
		else if (*p == ')')
			paren_depth--;
		else if (paren_depth == 0 && strncmp(p, op, op_len) == 0)
		{
			/* Make sure it's not part of another operator (e.g., || vs |) */
			if (op_len == 2 || (!isalnum(p[-1]) && !isalnum(p[op_len])))
				return (char *)p;
		}
		p++;
	}
	return NULL;
}

/* Parse a simple condition (no && or ||) */
ObbyScriptCondition *parse_simple_condition(const char *cond_str)
{
	ObbyScriptCondition *cond = safe_alloc(sizeof(ObbyScriptCondition));
	
	/* Trim whitespace */
	const char *start = cond_str;
	while (*start && isspace(*start)) start++;
	
	char *trimmed = strdup(start);
	char *end = trimmed + strlen(trimmed) - 1;
	while (end > trimmed && isspace(*end)) *end-- = '\0';
	
	/* Parse operators - check for special operators first (they have spaces around them) */
	char *op_pos = NULL;
	
	/* Check for special channel/user operators */
	if ((op_pos = strstr(trimmed, " hascap ")) != NULL)
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("hascap");
		char *value_start = op_pos + 8;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
	}
	else if ((op_pos = strstr(trimmed, " ischanop ")) != NULL)
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("ischanop");
		cond->value = strdup("$chan");
	}
	else if ((op_pos = strstr(trimmed, " isvoice ")) != NULL)
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isvoice");
		cond->value = strdup("$chan");
	}
	else if ((op_pos = strstr(trimmed, " ishalfop ")) != NULL)
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("ishalfop");
		cond->value = strdup("$chan");
	}
	else if ((op_pos = strstr(trimmed, " isadmin ")) != NULL)
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isadmin");
		cond->value = strdup("$chan");
	}
	else if ((op_pos = strstr(trimmed, " isowner ")) != NULL)
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isowner");
		cond->value = strdup("$chan");
	}
	/* New IRC-related operators */
	else if ((op_pos = strstr(trimmed, " isoper")) != NULL && (op_pos[7] == ')' || op_pos[7] == ' ' || op_pos[7] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isoper");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isinvisible")) != NULL && (op_pos[12] == ')' || op_pos[12] == ' ' || op_pos[12] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isinvisible");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isregnick")) != NULL && (op_pos[10] == ')' || op_pos[10] == ' ' || op_pos[10] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isregnick");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " ishidden")) != NULL && (op_pos[9] == ')' || op_pos[9] == ' ' || op_pos[9] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("ishidden");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " ishideoper")) != NULL && (op_pos[11] == ')' || op_pos[11] == ' ' || op_pos[11] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("ishideoper");
		cond->value = strdup("");
	}
	else if (((op_pos = strstr(trimmed, " issecure")) != NULL && (op_pos[9] == ')' || op_pos[9] == ' ' || op_pos[9] == '\0')) || 
	         ((op_pos = strstr(trimmed, " istls")) != NULL && (op_pos[6] == ')' || op_pos[6] == ' ' || op_pos[6] == '\0')))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("issecure");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isuline")) != NULL && (op_pos[8] == ')' || op_pos[8] == ' ' || op_pos[8] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isuline");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isloggedin")) != NULL && (op_pos[11] == ')' || op_pos[11] == ' ' || op_pos[11] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isloggedin");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isserver")) != NULL && (op_pos[9] == ')' || op_pos[9] == ' ' || op_pos[9] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isserver");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isquarantined")) != NULL && (op_pos[14] == ')' || op_pos[14] == ' ' || op_pos[14] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isquarantined");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isshunned")) != NULL && (op_pos[10] == ')' || op_pos[10] == ' ' || op_pos[10] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isshunned");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isvirus")) != NULL && (op_pos[8] == ')' || op_pos[8] == ' ' || op_pos[8] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isvirus");
		cond->value = strdup("");
	}
	else if ((op_pos = strstr(trimmed, " isinvited")) != NULL && (op_pos[10] == ')' || op_pos[10] == ' ' || op_pos[10] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isinvited");
		cond->value = strdup("$chan");
	}
	else if ((op_pos = strstr(trimmed, " isbanned")) != NULL && (op_pos[9] == ')' || op_pos[9] == ' ' || op_pos[9] == '\0'))
	{
		cond->variable = strdup("$client");
		cond->operator = strdup("isbanned");
		cond->value = strdup("$chan");
	}
	else if ((op_pos = strstr(trimmed, " hasaccess ")) != NULL)
	{
		/* $client hasaccess "voaq" */
		cond->variable = strdup("$client");
		cond->operator = strdup("hasaccess");
		char *value_start = op_pos + 11;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
	}
	else if ((op_pos = strstr(trimmed, " in ")) != NULL)
	{
		/* $client in $chan */
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup("in");
		
		char *value_start = op_pos + 4;
		while (*value_start && isspace(*value_start)) value_start++;
		char *value_str = strdup(value_start);
		char *value_end = value_str + strlen(value_str) - 1;
		while (value_end > value_str && isspace(*value_end)) *value_end-- = '\0';
		
		cond->value = value_str;
		free(var_str);
	}
	else if ((op_pos = strstr(trimmed, " insg ")) != NULL)
	{
		/* $client insg "security-group" */
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup("insg");
		
		char *value_start = op_pos + 6;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else if ((op_pos = strstr(trimmed, " !insg ")) != NULL)
	{
		/* $client !insg "security-group" */
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup("!insg");
		
		char *value_start = op_pos + 7;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else if ((op_pos = strstr(trimmed, " has ")) != NULL)
	{
		/* Variable has value */
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		cond->variable = strdup(var_str);
		cond->operator = strdup("has");
		
		char *value_start = op_pos + 5;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else if ((op_pos = strstr(trimmed, "<=")) != NULL)
	{
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup("<=");
		
		char *value_start = op_pos + 2;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else if ((op_pos = strstr(trimmed, ">=")) != NULL)
	{
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup(">=");
		
		char *value_start = op_pos + 2;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else if ((op_pos = strstr(trimmed, "==")) != NULL)
	{
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup("==");
		
		char *value_start = op_pos + 2;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else if ((op_pos = strstr(trimmed, "!=")) != NULL)
	{
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup("!=");
		
		char *value_start = op_pos + 2;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else if ((op_pos = strchr(trimmed, '<')) != NULL && op_pos[1] != '=')
	{
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup("<");
		
		char *value_start = op_pos + 1;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else if ((op_pos = strchr(trimmed, '>')) != NULL && op_pos[1] != '=')
	{
		int var_len = op_pos - trimmed;
		char *var_str = safe_alloc(var_len + 1);
		strncpy(var_str, trimmed, var_len);
		var_str[var_len] = '\0';
		
		char *var_trim = var_str;
		while (*var_trim && isspace(*var_trim)) var_trim++;
		char *var_end = var_trim + strlen(var_trim) - 1;
		while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
		
		cond->variable = strdup(var_trim);
		cond->operator = strdup(">");
		
		char *value_start = op_pos + 1;
		while (*value_start && isspace(*value_start)) value_start++;
		if (*value_start == '"')
		{
			value_start++;
			char *value_end = strchr(value_start, '"');
			if (value_end) *value_end = '\0';
		}
		cond->value = strdup(value_start);
		free(var_str);
	}
	else
	{
		/* Simple variable check (no operator) */
		cond->variable = strdup(trimmed);
		cond->operator = strdup("");
		cond->value = NULL;
	}
	
	free(trimmed);
	return cond;
}

/* Parse a boolean expression with &&, ||, and parentheses */
ObbyScriptBoolExpr *parse_bool_expression(const char *expr_str)
{
	if (!expr_str || !*expr_str)
		return NULL;
	
	/* Trim whitespace */
	const char *start = expr_str;
	while (*start && isspace(*start)) start++;
	
	char *trimmed = strdup(start);
	char *end = trimmed + strlen(trimmed) - 1;
	while (end > trimmed && isspace(*end)) *end-- = '\0';
	
	/* Check for outermost parentheses */
	if (trimmed[0] == '(' && trimmed[strlen(trimmed)-1] == ')')
	{
		/* Verify they're matching outermost parens */
		int paren_depth = 0;
		int is_outermost = 1;
		for (char *p = trimmed; *p; p++)
		{
			if (*p == '(') paren_depth++;
			else if (*p == ')') paren_depth--;
			
			if (paren_depth == 0 && p != trimmed + strlen(trimmed) - 1)
			{
				is_outermost = 0;
				break;
			}
		}
		
		if (is_outermost)
		{
			/* Strip outer parentheses and parse inner expression */
			char *inner = safe_alloc(strlen(trimmed) - 1);
			strncpy(inner, trimmed + 1, strlen(trimmed) - 2);
			inner[strlen(trimmed) - 2] = '\0';
			
			ObbyScriptBoolExpr *expr = safe_alloc(sizeof(ObbyScriptBoolExpr));
			expr->type = US_EXPR_PARENTHESES;
			expr->data.grouped = parse_bool_expression(inner);
			
			free(inner);
			free(trimmed);
			return expr;
		}
	}
	
	/* Look for || at top level (lowest precedence) */
	char *or_pos = find_top_level_operator(trimmed, "||");
	if (or_pos)
	{
		ObbyScriptBoolExpr *expr = safe_alloc(sizeof(ObbyScriptBoolExpr));
		expr->type = US_EXPR_OR;
		
		/* Split at || */
		*or_pos = '\0';
		expr->data.compound.left = parse_bool_expression(trimmed);
		expr->data.compound.right = parse_bool_expression(or_pos + 2);
		
		free(trimmed);
		return expr;
	}
	
	/* Look for && at top level (higher precedence than ||) */
	char *and_pos = find_top_level_operator(trimmed, "&&");
	if (and_pos)
	{
		ObbyScriptBoolExpr *expr = safe_alloc(sizeof(ObbyScriptBoolExpr));
		expr->type = US_EXPR_AND;
		
		/* Split at && */
		*and_pos = '\0';
		expr->data.compound.left = parse_bool_expression(trimmed);
		expr->data.compound.right = parse_bool_expression(and_pos + 2);
		
		free(trimmed);
		return expr;
	}
	
	/* No operators found - this is a simple condition */
	ObbyScriptBoolExpr *expr = safe_alloc(sizeof(ObbyScriptBoolExpr));
	expr->type = US_EXPR_SIMPLE;
	expr->data.simple = parse_simple_condition(trimmed);
	
	free(trimmed);
	return expr;
}

/* Evaluate a boolean expression */
int evaluate_bool_expr(ObbyScriptBoolExpr *expr, Client *client, Channel *channel)
{
	if (!expr)
		return 0;
	
	switch (expr->type)
	{
		case US_EXPR_SIMPLE:
			return evaluate_condition(expr->data.simple, client, channel);
			
		case US_EXPR_AND:
		{
			int left = evaluate_bool_expr(expr->data.compound.left, client, channel);
			if (!left) return 0;  /* Short-circuit AND */
			return evaluate_bool_expr(expr->data.compound.right, client, channel);
		}
		
		case US_EXPR_OR:
		{
			int left = evaluate_bool_expr(expr->data.compound.left, client, channel);
			if (left) return 1;  /* Short-circuit OR */
			return evaluate_bool_expr(expr->data.compound.right, client, channel);
		}
		
		case US_EXPR_PARENTHESES:
			return evaluate_bool_expr(expr->data.grouped, client, channel);
			
		default:
			return 0;
	}
}

ObbyScriptRule *parse_script_content(const char *content)
{
	ObbyScriptRule *rules = NULL, *current_rule = NULL;
	char *content_copy, *line_ptr, *saveptr;
	char *current_line;

	if (!content)
		return NULL;

	content_copy = strdup(content);
	if (!content_copy)
		return NULL;

	line_ptr = content_copy;
	current_line = strtok_r(line_ptr, "\n", &saveptr);
	
	while (current_line)
	{
		/* Skip whitespace */
		while (*current_line == ' ' || *current_line == '\t')
			current_line++;

		/* Skip empty lines and comments */
		if (*current_line == '\0' || strncmp(current_line, "//", 2) == 0)
		{
			current_line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}

		/* Look for function definitions: function $name($param1, $param2) { */
		if (strncmp(current_line, "function ", 9) == 0)
		{
			unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
				"Found function definition line: $line", 
				log_data_string("line", current_line));
				
			char *func_line = strdup(current_line);
			char *func_name_start = func_line + 9; /* Skip "function " */
			
			/* Skip whitespace */
			while (*func_name_start && isspace(*func_name_start)) func_name_start++;
			
			if (*func_name_start == '$')
			{
				func_name_start++; /* Skip $ */
				char *func_name_end = func_name_start;
				
				/* Find end of function name */
				while (*func_name_end && *func_name_end != '(' && !isspace(*func_name_end))
					func_name_end++;
				
				if (*func_name_end)
				{
					/* Extract function name without modifying the original string */
					int name_len = func_name_end - func_name_start;
					char *func_name = safe_alloc(name_len + 1);
					strncpy(func_name, func_name_start, name_len);
					func_name[name_len] = '\0';
					
					/* Search for parameters starting from the current position */
					char *search_start = func_name_end;
					
					unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
						"Searching for '(' in: '$text'", 
						log_data_string("text", search_start));
					
					/* Parse parameters - search from after the function name */
					char *param_start = strchr(search_start, '(');
					if (param_start)
					{
						unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
							"Found opening parenthesis for function $name", 
							log_data_string("name", func_name));
						
						param_start++; /* Skip '(' */
						char *param_end = strchr(param_start, ')');
						if (param_end)
						{
							unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
								"Found closing parenthesis for function $name", 
								log_data_string("name", func_name));
							
							/* Save the position after the closing parenthesis before null-terminating */
							char *after_params = param_end + 1;
							*param_end = '\0';
							
							/* Parse parameter list */
							char **parameters = NULL;
							int param_count = 0;
							
							unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
								"Parsing parameters: '$params'", 
								log_data_string("params", param_start));
							
							if (strlen(param_start) > 0)
							{
								/* Count parameters first */
								char *param_copy = strdup(param_start);
								char *param_token = strtok(param_copy, ",");
								while (param_token)
								{
									param_count++;
									param_token = strtok(NULL, ",");
								}
								free(param_copy);
								
								/* Allocate parameter array */
								if (param_count > 0)
								{
									parameters = safe_alloc(sizeof(char*) * param_count);
									param_copy = strdup(param_start);
									param_token = strtok(param_copy, ",");
									int i = 0;
									while (param_token && i < param_count)
									{
										/* Trim whitespace and remove $ */
										while (*param_token && isspace(*param_token)) param_token++;
										if (*param_token == '$') param_token++;
										char *param_end_ws = param_token + strlen(param_token) - 1;
										while (param_end_ws > param_token && isspace(*param_end_ws))
											*param_end_ws-- = '\0';
										parameters[i++] = strdup(param_token);
										param_token = strtok(NULL, ",");
									}
									free(param_copy);
								}
							}
							
							/* Look for opening brace */
							unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
								"Searching for opening brace after: '$text'", 
								log_data_string("text", after_params));
							
							char *brace = strchr(after_params, '{');
							if (brace)
							{
								unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
									"Found opening brace for function $name", 
									log_data_string("name", func_name));
								
								/* Parse function body */
								char function_buffer[4096] = {0};
								int brace_count = 1;
								
								current_line = strtok_r(NULL, "\n", &saveptr);
								
								while (current_line && brace_count > 0)
								{
									char *p = current_line;
									while (*p)
									{
										if (*p == '{') brace_count++;
										else if (*p == '}') brace_count--;
										p++;
									}
									
									if (brace_count > 0)
									{
										strlcat(function_buffer, current_line, sizeof(function_buffer));
										strlcat(function_buffer, "\n", sizeof(function_buffer));
									}
									
									if (brace_count > 0)
										current_line = strtok_r(NULL, "\n", &saveptr);
								}
								
								unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
									"Function body parsed: $body", 
									log_data_string("body", function_buffer));
								
								/* Parse function body actions */
								ObbyScriptAction *body = parse_action_block(function_buffer);
								
								unreal_log(ULOG_DEBUG, "obbyscript", "PARSE_DEBUG", NULL,
									"About to add function $name with $params parameters", 
									log_data_string("name", func_name),
									log_data_integer("params", param_count));
								
								/* Add function to global functions list */
								add_function(func_name, parameters, param_count, body);
							}
							else
							{
								unreal_log(ULOG_WARNING, "obbyscript", "PARSE_DEBUG", NULL,
									"No opening brace found for function $name", 
									log_data_string("name", func_name));
							}
							
							/* Free parameter array if allocated */
							if (parameters)
							{
								for (int i = 0; i < param_count; i++)
									free(parameters[i]);
								free(parameters);
							}
						}
					}
					
					free(func_name);
				}
			}
			
			free(func_line);
			current_line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}

		/* Look for event declarations: on EVENT:target:{ */
		if (strncmp(current_line, "on ", 3) == 0)
		{
			ObbyScriptRule *rule = safe_alloc(sizeof(ObbyScriptRule));
			char *event_line = strdup(current_line);
			char *event_part, *target_part;
			char *colon1, *colon2, *brace;
			
			/* Parse "on EVENT:target:{" */
			colon1 = strchr(event_line + 3, ':');
			if (colon1)
			{
				*colon1 = '\0';
				event_part = event_line + 3;
				
				/* Trim whitespace */
				while (*event_part == ' ') event_part++;
				
				colon2 = strchr(colon1 + 1, ':');
				if (colon2)
				{
					*colon2 = '\0';
					target_part = colon1 + 1;
					
					/* Trim whitespace */
					while (*target_part == ' ') target_part++;
					
					brace = strchr(colon2 + 1, '{');
					if (brace)
					{
						rule->event = parse_event_type(event_part);
						safe_strdup(rule->target, target_part);
						
						/* Parse action block */
						char action_buffer[4096] = {0};
						int brace_count = 1;
						
						current_line = strtok_r(NULL, "\n", &saveptr);
						
						while (current_line && brace_count > 0)
						{
							char *p = current_line;
							while (*p)
							{
								if (*p == '{') brace_count++;
								else if (*p == '}') brace_count--;
								p++;
							}
							
							if (brace_count > 0)
							{
								strlcat(action_buffer, current_line, sizeof(action_buffer));
								strlcat(action_buffer, "\n", sizeof(action_buffer));
							}
							
							if (brace_count > 0)
								current_line = strtok_r(NULL, "\n", &saveptr);
						}
						
						rule->actions = parse_action_block(action_buffer);

						rule->next = NULL;
						if (!rules)
							rules = rule;
						else
							current_rule->next = rule;
						current_rule = rule;
					}
					else
					{
						safe_free(rule);
					}
				}
				else
				{
					safe_free(rule);
				}
			}
			else
			{
				safe_free(rule);
			}
			
			free(event_line);
		}
		/* Look for new command declarations: new COMMAND:target:{ */
		else if (strncmp(current_line, "new ", 4) == 0)
		{
			ObbyScriptRule *rule = safe_alloc(sizeof(ObbyScriptRule));
			char *event_line = strdup(current_line);
			char *event_part, *target_part;
			char *colon1, *colon2, *brace;
			
			/* Parse "new COMMAND:target:{" */
			colon1 = strchr(event_line + 4, ':');
			if (colon1)
			{
				*colon1 = '\0';
				event_part = event_line + 4;
				
				/* Trim whitespace */
				while (*event_part == ' ') event_part++;
				
				colon2 = strchr(colon1 + 1, ':');
				if (colon2)
				{
					*colon2 = '\0';
					target_part = colon1 + 1;
					
					/* Trim whitespace */
					while (*target_part == ' ') target_part++;
					
					brace = strchr(colon2 + 1, '{');
					if (brace)
					{
						/* Only handle COMMAND events */
						if (strcasecmp(event_part, "COMMAND") == 0)
						{
							rule->event = US_EVENT_COMMAND_NEW;
							safe_strdup(rule->target, target_part);
							
							/* Parse action block */
							char action_buffer[4096] = {0};
							int brace_count = 1;
							
							current_line = strtok_r(NULL, "\n", &saveptr);
							
							while (current_line && brace_count > 0)
							{
								char *p = current_line;
								while (*p)
								{
									if (*p == '{') brace_count++;
									else if (*p == '}') brace_count--;
									p++;
								}
								
								if (brace_count > 0)
								{
									strlcat(action_buffer, current_line, sizeof(action_buffer));
									strlcat(action_buffer, "\n", sizeof(action_buffer));
								}
								
								if (brace_count > 0)
									current_line = strtok_r(NULL, "\n", &saveptr);
							}
							
							rule->actions = parse_action_block(action_buffer);

							rule->next = NULL;
							if (!rules)
								rules = rule;
							else
								current_rule->next = rule;
							current_rule = rule;
						}
						else
						{
							safe_free(rule);
						}
					}
					else
					{
						safe_free(rule);
					}
				}
				else
				{
					safe_free(rule);
				}
			}
			else
			{
				safe_free(rule);
			}
			
			free(event_line);
		}

		current_line = strtok_r(NULL, "\n", &saveptr);
	}

	free(content_copy);
	return rules;
}

ObbyScriptEventType parse_event_type(const char *event_str)
{
	if (!event_str)
		return US_EVENT_MAX;

	if (strcasecmp(event_str, "START") == 0)
		return US_EVENT_START;
	else if (strcasecmp(event_str, "CONNECT") == 0)
		return US_EVENT_CONNECT;
	else if (strcasecmp(event_str, "QUIT") == 0)
		return US_EVENT_QUIT;
	else if (strcasecmp(event_str, "CAN_JOIN") == 0)
		return US_EVENT_CAN_JOIN;
	else if (strcasecmp(event_str, "JOIN") == 0)
		return US_EVENT_JOIN;
	else if (strcasecmp(event_str, "PART") == 0)
		return US_EVENT_PART;
	else if (strcasecmp(event_str, "KICK") == 0)
		return US_EVENT_KICK;
	else if (strcasecmp(event_str, "NICK") == 0)
		return US_EVENT_NICK;
	else if (strcasecmp(event_str, "PRIVMSG") == 0)
		return US_EVENT_PRIVMSG;
	else if (strcasecmp(event_str, "NOTICE") == 0)
		return US_EVENT_NOTICE;
	else if (strcasecmp(event_str, "TOPIC") == 0)
		return US_EVENT_TOPIC;
	else if (strcasecmp(event_str, "MODE") == 0)
		return US_EVENT_MODE;
	else if (strcasecmp(event_str, "INVITE") == 0)
		return US_EVENT_INVITE;
	else if (strcasecmp(event_str, "KNOCK") == 0)
		return US_EVENT_KNOCK;
	else if (strcasecmp(event_str, "AWAY") == 0)
		return US_EVENT_AWAY;
	else if (strcasecmp(event_str, "OPER") == 0)
		return US_EVENT_OPER;
	else if (strcasecmp(event_str, "KILL") == 0)
		return US_EVENT_KILL;
	else if (strcasecmp(event_str, "UMODE") == 0)
		return US_EVENT_UMODE_CHANGE;
	else if (strcasecmp(event_str, "CHANMODE") == 0)
		return US_EVENT_CHANMODE;
	else if (strcasecmp(event_str, "CHANNEL_CREATE") == 0)
		return US_EVENT_CHANNEL_CREATE;
	else if (strcasecmp(event_str, "CHANNEL_DESTROY") == 0)
		return US_EVENT_CHANNEL_DESTROY;
	else if (strcasecmp(event_str, "WHOIS") == 0)
		return US_EVENT_WHOIS;
	else if (strcasecmp(event_str, "REHASH") == 0)
		return US_EVENT_REHASH;
	else if (strcasecmp(event_str, "ACCOUNT_LOGIN") == 0)
		return US_EVENT_ACCOUNT_LOGIN;
	else if (strcasecmp(event_str, "PRE_COMMAND") == 0)
		return US_EVENT_PRE_COMMAND;
	else if (strcasecmp(event_str, "POST_COMMAND") == 0)
		return US_EVENT_POST_COMMAND;
	else if (strcasecmp(event_str, "TKL_ADD") == 0)
		return US_EVENT_TKL_ADD;
	else if (strcasecmp(event_str, "TKL_DEL") == 0)
		return US_EVENT_TKL_DEL;
	else if (strcasecmp(event_str, "SPAMFILTER") == 0)
		return US_EVENT_SPAMFILTER;
	else if (strcasecmp(event_str, "COMMAND") == 0)
		return US_EVENT_COMMAND_OVERRIDE;

	return US_EVENT_MAX;
}

char *obbyscript_replace_string(const char *haystack, const char *needle, const char *replacement)
{
	char *result;
	char *pos;
	int needle_len, replacement_len, haystack_len;
	int count = 0;
	
	if (!haystack || !needle || !replacement)
		return NULL;
		
	needle_len = strlen(needle);
	replacement_len = strlen(replacement);
	haystack_len = strlen(haystack);
	
	/* Count occurrences */
	pos = (char *)haystack;
	while ((pos = strstr(pos, needle)) != NULL)
	{
		count++;
		pos += needle_len;
	}
	
	if (count == 0)
		return strdup(haystack);
	
	/* Allocate result buffer */
	result = safe_alloc(haystack_len + count * (replacement_len - needle_len) + 1);
	
	/* Replace occurrences */
	char *result_pos = result;
	pos = (char *)haystack;
	
	while ((pos = strstr(pos, needle)) != NULL)
	{
		/* Copy text before needle */
		int prefix_len = pos - haystack;
		if (prefix_len > 0)
		{
			memcpy(result_pos, haystack, prefix_len);
			result_pos += prefix_len;
		}
		
		/* Copy replacement */
		if (replacement_len > 0)
		{
			memcpy(result_pos, replacement, replacement_len);
			result_pos += replacement_len;
		}
		
		/* Move past needle */
		haystack = pos + needle_len;
		pos = (char *)haystack;
	}
	
	/* Copy remaining text */
	strcpy(result_pos, haystack);
	
	return result;
}

ObbyScriptAction *parse_action_block(const char *content)
{
	ObbyScriptAction *actions = NULL, *current_action = NULL;
	char *content_copy, *line_ptr, *saveptr;
	char *current_line;
	
	/* State tracking for nested IF blocks */
	ObbyScriptAction *current_if_action = NULL;
	ObbyScriptAction *nested_head = NULL;
	ObbyScriptAction *nested_tail = NULL;
	ObbyScriptAction *else_head = NULL;
	ObbyScriptAction *else_tail = NULL;
	int inside_if_block = 0;
	int inside_else_block = 0;
	
	/* Stack for nested IF contexts */
	#define MAX_IF_DEPTH 10
	typedef struct {
		ObbyScriptAction *if_action;
		ObbyScriptAction *nested_head;
		ObbyScriptAction *nested_tail;
		ObbyScriptAction *else_head;
		ObbyScriptAction *else_tail;
		int inside_if;
		int inside_else;
	} IFContext;
	
	IFContext if_stack[MAX_IF_DEPTH];
	int if_depth = 0;
	
	/* State tracking for nested LOOP blocks (while/for) - separate from IF blocks */
	ObbyScriptAction *current_loop_action = NULL;
	ObbyScriptAction *loop_nested_head = NULL;
	ObbyScriptAction *loop_nested_tail = NULL;
	int inside_loop_block = 0;
	
	/* Stack for nested LOOP contexts */
	#define MAX_LOOP_DEPTH 10
	typedef struct {
		ObbyScriptAction *loop_action;
		ObbyScriptAction *nested_head;
		ObbyScriptAction *nested_tail;
		int inside_loop;
	} LOOPContext;
	
	LOOPContext loop_stack[MAX_LOOP_DEPTH];
	int loop_depth = 0;

	if (!content)
		return NULL;

	content_copy = strdup(content);
	if (!content_copy)
		return NULL;

	line_ptr = content_copy;
	current_line = strtok_r(line_ptr, "\n", &saveptr);
	
	while (current_line)
	{
		/* Skip whitespace */
		while (*current_line == ' ' || *current_line == '\t')
			current_line++;

		/* Skip empty lines and comments */
		if (*current_line == '\0' || strncmp(current_line, "//", 2) == 0)
		{
			current_line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}
		
		/* Handle closing braces for IF/ELSE blocks and LOOP blocks */
		/* Handle closing braces for LOOP blocks (while/for) - but not if inside IF/ELSE */
		if (*current_line == '}' && inside_loop_block && !inside_if_block && !inside_else_block)
		{
			fprintf(stderr, "[PARSE_DEBUG] Closing brace for LOOP, loop_depth=%d, nested_head=%p\n", 
				loop_depth, loop_nested_head);
			
			/* End of loop block - save nested actions */
			if (current_loop_action)
			{
				current_loop_action->nested_actions = loop_nested_head;
				fprintf(stderr, "[PARSE_DEBUG] Linked %p nested actions to loop action '%s'\n",
					loop_nested_head, current_loop_action->function ? current_loop_action->function : "NULL");
			}
			
			/* Pop loop stack to restore parent context if nested */
			if (loop_depth > 0)
			{
				loop_depth--;
				current_loop_action = loop_stack[loop_depth].loop_action;
				loop_nested_head = loop_stack[loop_depth].nested_head;
				loop_nested_tail = loop_stack[loop_depth].nested_tail;
				inside_loop_block = loop_stack[loop_depth].inside_loop;
				fprintf(stderr, "[PARSE_DEBUG] Popped loop stack, now depth=%d\n", loop_depth);
			}
			else
			{
				/* Top-level loop ending */
				inside_loop_block = 0;
				current_loop_action = NULL;
				loop_nested_head = NULL;
				loop_nested_tail = NULL;
				fprintf(stderr, "[PARSE_DEBUG] Top-level loop ended, cleared loop state\n");
			}
			
			current_line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}
		
		/* Handle closing braces for IF/ELSE blocks (including when inside loops) */
		if (*current_line == '}')
		{
			unreal_log(ULOG_INFO, "obbyscript", "DEBUG_BRACE_CHECK", NULL,
				"Found closing brace: if=$if, else=$else, loop=$loop",
				log_data_integer("if", inside_if_block),
				log_data_integer("else", inside_else_block),
				log_data_integer("loop", inside_loop_block));
		}
		if (*current_line == '}' && (inside_if_block || inside_else_block))
		{
			/* Check for } else if on the same line (only relevant for IF blocks) */
			char *trimmed_current = current_line;
			while (*trimmed_current && isspace(*trimmed_current)) trimmed_current++;
			
			if (strstr(trimmed_current, "} else if (") != NULL && inside_if_block)
			{
				
				if (inside_else_block)
				{
					/* End of else block */
					if (current_if_action)
					{
						current_if_action->else_actions = nested_head;
					}
					
					/* Pop stack to restore parent context if nested */
					if (if_depth > 0)
					{
						if_depth--;
						current_if_action = if_stack[if_depth].if_action;
						nested_head = if_stack[if_depth].nested_head;
						nested_tail = if_stack[if_depth].nested_tail;
						else_head = if_stack[if_depth].else_head;
						else_tail = if_stack[if_depth].else_tail;
						inside_if_block = if_stack[if_depth].inside_if;
						inside_else_block = if_stack[if_depth].inside_else;
					}
					else
					{
						/* Reset all state */
						inside_if_block = 0;
						inside_else_block = 0;
						current_if_action = NULL;
						nested_head = NULL;
						nested_tail = NULL;
					}
				}
				else if (inside_if_block)
				{
					/* End of if block - save nested actions */
					if (current_if_action)
					{
						current_if_action->nested_actions = nested_head;
					}
					
					/* Pop stack to restore parent context if nested */
					if (if_depth > 0)
					{
						if_depth--;
						current_if_action = if_stack[if_depth].if_action;
						nested_head = if_stack[if_depth].nested_head;
						nested_tail = if_stack[if_depth].nested_tail;
						else_head = if_stack[if_depth].else_head;
						else_tail = if_stack[if_depth].else_tail;
						inside_if_block = if_stack[if_depth].inside_if;
						inside_else_block = if_stack[if_depth].inside_else;
					}
					else
					{
						/* Reset state for new if */
						inside_if_block = 0;
						current_if_action = NULL;
						nested_head = NULL;
						nested_tail = NULL;
					}
				}
				
				/* Process the } else if as a new if statement */
				char *else_if_start = strstr(trimmed_current, "else if (");
				if (else_if_start)
				{
					current_line = else_if_start;
					/* Continue to process this as an if statement */
				}
				else
				{
					current_line = strtok_r(NULL, "\n", &saveptr);
				}
				continue;
			}
			
			if (inside_else_block)
			{
				/* End of else block */
				if (current_if_action)
				{
					current_if_action->else_actions = else_head;
				}
				
				/* Pop stack to restore parent context if nested */
				if (if_depth > 0)
				{
					if_depth--;
					current_if_action = if_stack[if_depth].if_action;
					nested_head = if_stack[if_depth].nested_head;
					nested_tail = if_stack[if_depth].nested_tail;
					else_head = if_stack[if_depth].else_head;
					else_tail = if_stack[if_depth].else_tail;
					inside_if_block = if_stack[if_depth].inside_if;
					inside_else_block = if_stack[if_depth].inside_else;
				}
				else
				{
					/* Reset all state */
					inside_if_block = 0;
					inside_else_block = 0;
					current_if_action = NULL;
					nested_head = NULL;
					nested_tail = NULL;
					else_head = NULL;
					else_tail = NULL;
				}
			}
			else if (inside_if_block)
			{
				/* End of if block - save nested actions and look for else */
				if (current_if_action)
				{
					unreal_log(ULOG_INFO, "obbyscript", "DEBUG_IF_NESTED_LINK", NULL,
						"Linking nested actions to IF: function=$function, has_nested=$has, if_depth=$depth, in_loop=$loop",
						log_data_string("function", current_if_action->function ? current_if_action->function : "NULL"),
						log_data_integer("has", nested_head ? 1 : 0),
						log_data_integer("depth", if_depth),
						log_data_integer("loop", inside_loop_block));
					current_if_action->nested_actions = nested_head;
				}
				
				/* Reset nested actions but keep if context for else detection */
				nested_head = NULL;
				nested_tail = NULL;
				/* Don't reset inside_if_block or current_if_action yet - check for else */
				
				/* Look ahead for else statement on next line or same line */
				char *next_line = strtok_r(NULL, "\n", &saveptr);
				if (next_line)
				{
					char *trimmed_next = next_line;
					while (*trimmed_next && isspace(*trimmed_next)) trimmed_next++;
					
					unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_ELSE_LOOKAHEAD", NULL,
						"After if block closing brace, checking next line: '$line'",
						log_data_string("line", trimmed_next));
					
					/* Check for else statements */
					if (strncmp(trimmed_next, "else if (", 9) == 0)
					{
						
						/* Create a new IF action for the else if */
						ObbyScriptAction *elseif_action = safe_alloc(sizeof(ObbyScriptAction));
						elseif_action->type = US_ACTION_IF;
						safe_strdup(elseif_action->function, "if");
						
						/* Parse the condition from the else if line */
						char *condition_start = strstr(trimmed_next, "(") + 1;
						char *condition_end = strrchr(trimmed_next, ')');
						
						if (condition_end && condition_start < condition_end)
						{
							int condition_len = condition_end - condition_start;
							char *condition_str = safe_alloc(condition_len + 1);
							strncpy(condition_str, condition_start, condition_len);
							condition_str[condition_len] = '\0';
							
							/* Parse the condition like a regular if */
							elseif_action->condition = safe_alloc(sizeof(ObbyScriptCondition));
							
							/* Find operator and parse condition components */
							char *op_pos = strstr(condition_str, "==");
							int is_equal = 1;
							if (!op_pos)
							{
								op_pos = strstr(condition_str, "!=");
								is_equal = 0;
							}
							
							if (op_pos)
							{
								/* Extract variable name */
								int var_len = op_pos - condition_str;
								char *var_name = safe_alloc(var_len + 1);
								strncpy(var_name, condition_str, var_len);
								var_name[var_len] = '\0';
								
								/* Trim whitespace */
								char *var_trim = var_name;
								while (*var_trim && isspace(*var_trim)) var_trim++;
								char *var_end = var_trim + strlen(var_trim) - 1;
								while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
								
								elseif_action->condition->variable = strdup(var_trim);
								elseif_action->condition->operator = strdup(is_equal ? "==" : "!=");
								
								/* Extract value */
								char *value_start = op_pos + 2;
								while (*value_start && isspace(*value_start)) value_start++;
								
								if (*value_start == '"')
								{
									value_start++;
									char *value_end = strchr(value_start, '"');
									if (value_end)
									{
										int value_len = value_end - value_start;
										char *value_str = safe_alloc(value_len + 1);
										strncpy(value_str, value_start, value_len);
										value_str[value_len] = '\0';
										elseif_action->condition->value = value_str;
									}
								}
								else
								{
									elseif_action->condition->value = strdup(value_start);
								}
								
								free(var_name);
							}
							
							free(condition_str);
						}
						
						/* Set this else if action as the else_actions of the current if */
						if (current_if_action)
						{
							current_if_action->else_actions = elseif_action;
						}
						
						/* Update context to parse the new else if block */
						current_if_action = elseif_action;
						inside_if_block = 1;
						inside_else_block = 0;
						nested_head = NULL;
						nested_tail = NULL;
						
						current_line = strtok_r(NULL, "\n", &saveptr);
						continue;
					}
					else if (strncmp(trimmed_next, "else {", 6) == 0 || strncmp(trimmed_next, "else{", 5) == 0)
					{
						inside_else_block = 1;
						else_head = NULL;
						else_tail = NULL;
						current_line = strtok_r(NULL, "\n", &saveptr);
						continue;
					}
					else if (strcmp(trimmed_next, "else") == 0)
					{
						/* Standalone else - look for opening brace on following line */
						char *brace_line = strtok_r(NULL, "\n", &saveptr);
						if (brace_line)
						{
							char *trimmed_brace = brace_line;
							while (*trimmed_brace && isspace(*trimmed_brace)) trimmed_brace++;
							
							if (strcmp(trimmed_brace, "{") == 0)
							{
								inside_else_block = 1;
								else_head = NULL;
								else_tail = NULL;
								current_line = strtok_r(NULL, "\n", &saveptr);
								continue;
							}
							else
							{
								/* Not an else block */
								inside_if_block = 0;
								current_if_action = NULL;
								current_line = brace_line;
								/* Process brace_line normally */
							}
						}
						else
						{
							/* End of input after else */
							inside_if_block = 0;
							current_if_action = NULL;
							break;
						}
					}
					else
					{
						/* No else statement - end if context */
						unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_NO_ELSE", NULL,
							"No else statement found - ending if context");
						
						/* Check if we need to pop the IF stack */
						if (if_depth > 0)
						{
							unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_IF_STACK_POP", NULL,
								"Popping IF context from stack, depth was: $depth",
								log_data_integer("depth", if_depth));
							
							/* Restore previous IF context */
							if_depth--;
							current_if_action = if_stack[if_depth].if_action;
							nested_head = if_stack[if_depth].nested_head;
							nested_tail = if_stack[if_depth].nested_tail;
							else_head = if_stack[if_depth].else_head;
							else_tail = if_stack[if_depth].else_tail;
							inside_if_block = if_stack[if_depth].inside_if;
							inside_else_block = if_stack[if_depth].inside_else;
						}
						else
						{
							/* Top-level IF ending */
							inside_if_block = 0;
							current_if_action = NULL;
						}
						
						current_line = next_line;
						/* Process next_line normally */
					}
				}
				else
				{
					/* End of input */
					if (if_depth > 0)
					{
						/* Pop context */
						if_depth--;
						current_if_action = if_stack[if_depth].if_action;
						nested_head = if_stack[if_depth].nested_head;
						nested_tail = if_stack[if_depth].nested_tail;
						else_head = if_stack[if_depth].else_head;
						else_tail = if_stack[if_depth].else_tail;
						inside_if_block = if_stack[if_depth].inside_if;
						inside_else_block = if_stack[if_depth].inside_else;
					}
					else
					{
						inside_if_block = 0;
						current_if_action = NULL;
					}
					break;
				}
			}
			
			current_line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}

		/* Parse various command types */
		ObbyScriptAction *action = NULL;
		
		fprintf(stderr, "[PARSE_DEBUG] Parsing line: '%s'\n", current_line);
		
		/* Function calls: $functionname($arg1, $arg2) */
		if (is_function_call(current_line))
		{
			fprintf(stderr, "[PARSE_DEBUG] Line identified as function call\n");
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_FUNCTION_CALL;
			
			/* Parse function name and arguments */
			char *line_copy = strdup(current_line);
			char *func_start = line_copy;
			while (*func_start && isspace(*func_start)) func_start++;
			
			if (*func_start == '$')
			{
				func_start++; /* Skip $ */
				char *paren_pos = strchr(func_start, '(');
				if (paren_pos)
				{
					*paren_pos = '\0';
					safe_strdup(action->function, func_start);
					
					/* Parse arguments */
					char *args_start = paren_pos + 1;
					char *args_end = strchr(args_start, ')');
					if (args_end)
					{
						*args_end = '\0';
						
						/* Count and parse arguments */
						char **arg_tokens = NULL;
						int argc = 0;
						
						if (strlen(args_start) > 0)
						{
							/* Simple comma-separated parsing */
							char *args_copy = strdup(args_start);
							char *token = strtok(args_copy, ",");
							while (token)
							{
								argc++;
								token = strtok(NULL, ",");
							}
							free(args_copy);
							
							if (argc > 0)
							{
								arg_tokens = safe_alloc(sizeof(char*) * argc);
								args_copy = strdup(args_start);
								token = strtok(args_copy, ",");
								int i = 0;
								while (token && i < argc)
								{
									/* Trim whitespace */
									while (*token && isspace(*token)) token++;
									char *end = token + strlen(token) - 1;
									while (end > token && isspace(*end)) *end-- = '\0';
									arg_tokens[i++] = strdup(token);
									token = strtok(NULL, ",");
								}
								free(args_copy);
							}
						}
						
						action->argc = argc;
						action->args = arg_tokens;
					}
				}
			}
			free(line_copy);
		}
		/* Arithmetic operations: %var++, %var--, %var += 1, %var = %var + 1 */
		else if (is_arithmetic_operation(current_line))
		{
			fprintf(stderr, "[PARSE_DEBUG] Line identified as arithmetic operation: '%s'\n", current_line);
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_ARITHMETIC;
			safe_strdup(action->function, "arithmetic");
			
			action->argc = 1;
			action->args = safe_alloc(sizeof(char*));
			action->args[0] = strdup(current_line);
		}
		/* Variable declaration/assignment: var %name value or %name = value */
		else if (strncmp(current_line, "var ", 4) == 0 || 
		    strncmp(current_line, "const var ", 10) == 0 ||
		    (current_line[0] == '%' && strchr(current_line, '=') != NULL && !is_arithmetic_operation(current_line)))
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_VAR;
			safe_strdup(action->function, "var");
			
			/* Parse the variable command with proper quote handling */
			char *line_copy = strdup(current_line);
			char *tokens[10];
			int argc = 0;
			char *p = line_copy;
			
			/* Skip leading whitespace */
			while (*p && isspace(*p)) p++;
			
			while (*p && argc < 10)
			{
				char *token_start = p;
				char *token_end;
				
				if (*p == '"')
				{
					/* Quoted string - find closing quote */
					p++; /* Skip opening quote */
					token_start = p;
					while (*p && *p != '"') p++;
					token_end = p;
					if (*p == '"') p++; /* Skip closing quote */
				}
				else if (*p == '[')
				{
					/* Array literal - find closing bracket, preserving the whole thing */
					token_start = p;
					int bracket_depth = 0;
					while (*p)
					{
						if (*p == '[') bracket_depth++;
						else if (*p == ']')
						{
							bracket_depth--;
							if (bracket_depth == 0)
							{
								p++;
								break;
							}
						}
						else if (*p == '"')
						{
							/* Skip quoted strings inside array */
							p++;
							while (*p && *p != '"')
							{
								if (*p == '\\' && *(p+1)) p++;
								p++;
							}
							if (*p == '"') p++;
							continue;
						}
						p++;
					}
					token_end = p;
				}
				else
				{
					/* Unquoted token - find space or end */
					while (*p && !isspace(*p)) p++;
					token_end = p;
				}
				
				/* Create token */
				int token_len = token_end - token_start;
				char *token = safe_alloc(token_len + 1);
				strncpy(token, token_start, token_len);
				token[token_len] = '\0';
				tokens[argc++] = token;
				
				/* Skip whitespace */
				while (*p && isspace(*p)) p++;
			}
			
			action->argc = argc;
			if (argc > 0) {
				action->args = safe_alloc(sizeof(char*) * argc);
				for (int i = 0; i < argc; i++)
					action->args[i] = tokens[i];
			}
			
			free(line_copy);
		}
		/* ISUPPORT token: isupport TOKENAME=value */
		else if (strncmp(current_line, "isupport ", 9) == 0)
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_ISUPPORT;
			safe_strdup(action->function, "isupport");
			
			char *token_def = current_line + 9;
			action->argc = 1;
			action->args = safe_alloc(sizeof(char*));
			action->args[0] = strdup(token_def);
		}
		/* CAP capability: cap capname */
		else if (strncmp(current_line, "cap ", 4) == 0)
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_CAP;
			safe_strdup(action->function, "cap");
			
			char *cap_name = current_line + 4;
			action->argc = 1;
			action->args = safe_alloc(sizeof(char*));
			action->args[0] = strdup(cap_name);
		}
		/* Legacy sendnotice support */
		else if (strncmp(current_line, "sendnotice ", 11) == 0)
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_SENDNOTICE;
			safe_strdup(action->function, "sendnotice");
			
			/* Parse arguments - handle quoted strings properly */
			char *args_str = current_line + 11;
			char *arg_tokens[10];
			int argc = 0;
			char *p = args_str;
			
			/* Skip leading whitespace */
			while (*p && isspace(*p)) p++;
			
			while (*p && argc < 10)
			{
				char *token_start = p;
				char *token_end;
				
				if (*p == '"')
				{
					/* Quoted string - find closing quote */
					p++; /* Skip opening quote */
					token_start = p;
					while (*p && *p != '"') p++;
					token_end = p;
					if (*p == '"') p++; /* Skip closing quote */
				}
				else
				{
					/* Unquoted token - find space or end */
					while (*p && !isspace(*p)) p++;
					token_end = p;
				}
				
				/* Extract token */
				if (token_end > token_start)
				{
					int len = token_end - token_start;
					char *token = safe_alloc(len + 1);
					strncpy(token, token_start, len);
					token[len] = '\0';
					arg_tokens[argc++] = token;
				}
				
				/* Skip whitespace */
				while (*p && isspace(*p)) p++;
			}
			
			action->argc = argc;
			if (argc > 0)
			{
				action->args = safe_alloc(sizeof(char*) * argc);
				for (int i = 0; i < argc; i++)
					action->args[i] = arg_tokens[i];
			}
		}
		/* Break statement for loops */
		else if (strcmp(current_line, "break") == 0)
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_BREAK;
			safe_strdup(action->function, "break");
			action->argc = 0;
			action->args = NULL;
		}
		/* Continue statement for loops */
		else if (strcmp(current_line, "continue") == 0)
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_CONTINUE;
			safe_strdup(action->function, "continue");
			action->argc = 0;
			action->args = NULL;
		}
		/* Return statement support for CAN_JOIN events */
		else if (strncmp(current_line, "return ", 7) == 0)
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_RETURN;
			safe_strdup(action->function, "return");
			
			/* Parse the return value */
			char *return_value = current_line + 7;
			
			/* Skip leading whitespace */
			while (*return_value && isspace(*return_value)) return_value++;
			
			/* Check for boolean values */
			if (strncmp(return_value, "$true", 5) == 0)
			{
				action->argc = 1;
				action->args = safe_alloc(sizeof(char*));
				action->args[0] = strdup("$true");
			}
			else if (strncmp(return_value, "$false", 6) == 0)
			{
				action->argc = 1;
				action->args = safe_alloc(sizeof(char*));
				action->args[0] = strdup("$false");
			}
			else if (*return_value == '"')
			{
				/* Extract quoted string for custom error messages */
				return_value++; /* Skip opening quote */
				char *end_quote = strchr(return_value, '"');
				if (end_quote)
				{
					int len = end_quote - return_value;
					char *error_msg = safe_alloc(len + 1);
					strncpy(error_msg, return_value, len);
					error_msg[len] = '\0';
					
					action->argc = 1;
					action->args = safe_alloc(sizeof(char*));
					action->args[0] = error_msg;
				}
			}
		}
		/* If statement: if ($client.name == "value") action */
		else if (strncmp(current_line, "if (", 4) == 0)
		{
			/* Parse the IF statement */
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_IF;
			safe_strdup(action->function, "if");
			
			/* Parse the condition inside the parentheses */
			char *condition_start = current_line + 4; /* Skip "if (" */
			
			/* Find the closing parenthesis - need to find the LAST one before the opening brace */
			char *brace_pos = strchr(condition_start, '{');
			char *condition_end = NULL;
			
			if (brace_pos)
			{
				/* Search backwards from the brace to find the last ')' */
				char *search_pos = brace_pos - 1;
				while (search_pos > condition_start && *search_pos != ')')
				{
					search_pos--;
				}
				if (*search_pos == ')')
				{
					condition_end = search_pos;
				}
			}
			
			if (!condition_end)
			{
				/* Fallback: use strchr if we couldn't find it with the above method */
				condition_end = strchr(condition_start, ')');
			}
			
			if (condition_end)
			{
				int condition_len = condition_end - condition_start;
				char *condition_str = safe_alloc(condition_len + 1);
				strncpy(condition_str, condition_start, condition_len);
				condition_str[condition_len] = '\0';
				
				/* Debug: Log what condition string was parsed */
				unreal_log(ULOG_DEBUG, "obbyscript", "CONDITION_PARSE_DEBUG", NULL,
					"Parsed condition string for evaluation");
				
				/* NEW: Parse as boolean expression to support || and && */
				action->bool_expr = parse_bool_expression(condition_str);
				
				/* Do NOT set action->condition to avoid double-free issues */
				/* The execute_script_action will check bool_expr first */
				action->condition = NULL;
				
				free(condition_str);
				
				/* OLD CONDITION PARSING REMOVED - Using new bool_expr system */
				
				/* Parse the action after the closing parenthesis */
				char *action_start = condition_end + 1;
				while (*action_start && isspace(*action_start)) action_start++;
				
				if (*action_start == '{')
				{
					/* Check if we're inside a loop, IF, or ELSE block */
					if (inside_loop_block || inside_if_block || inside_else_block)
					{
						/* This IF is nested inside something - save current context to stack */
						unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_NESTED_IF_PUSH", NULL,
							"Pushing IF context to stack (inside_loop=$loop, inside_if=$if), depth will be: $depth",
							log_data_integer("loop", inside_loop_block),
							log_data_integer("if", inside_if_block),
							log_data_integer("depth", if_depth + 1));
						
						if (if_depth < MAX_IF_DEPTH)
						{
							/* Save current context */
							if_stack[if_depth].if_action = current_if_action;
							if_stack[if_depth].nested_head = nested_head;
							if_stack[if_depth].nested_tail = nested_tail;
							if_stack[if_depth].else_head = else_head;
							if_stack[if_depth].else_tail = else_tail;
							if_stack[if_depth].inside_if = inside_if_block;
							if_stack[if_depth].inside_else = inside_else_block;
							if_depth++;
							
							/* Add the nested IF to the appropriate list */
							action->next = NULL;
							
							/* If inside a loop, add to loop's nested actions */
							if (inside_loop_block)
							{
								if (!loop_nested_head)
									loop_nested_head = loop_nested_tail = action;
								else
								{
									loop_nested_tail->next = action;
									loop_nested_tail = action;
								}
							}
							/* If inside else block, add to else list */
							else if (inside_else_block)
							{
								if (!else_head)
									else_head = else_tail = action;
								else
								{
									else_tail->next = action;
									else_tail = action;
								}
								/* Update stack with new tail */
								if_stack[if_depth-1].else_tail = else_tail;
							}
							/* If inside if block, add to nested list */
							else
							{
								if (!nested_head)
									nested_head = nested_tail = action;
								else
								{
									nested_tail->next = action;
									nested_tail = action;
								}
								/* Update stack with new tail */
								if_stack[if_depth-1].nested_tail = nested_tail;
							}
							
							/* Start new context for this nested IF */
							current_if_action = action;
							nested_head = NULL;
							nested_tail = NULL;
							else_head = NULL;
							else_tail = NULL;
							inside_if_block = 1;
							inside_else_block = 0;
						}
						
						/* Continue to next line to collect this nested IF's actions */
						current_line = strtok_r(NULL, "\n", &saveptr);
						continue;
					}
					else
					{
						/* Top-level IF - add to main list and set state */					
						action->next = NULL;
						if (!actions)
							actions = action;
						else
							current_action->next = action;
						current_action = action;
						
						/* Now set state for collecting nested actions */
						inside_if_block = 1;
						current_if_action = action;
						nested_head = NULL;
						nested_tail = NULL;
						else_head = NULL;
						else_tail = NULL;
						
						/* Continue to next line - don't process this action again in the main logic */
						current_line = strtok_r(NULL, "\n", &saveptr);
						continue;
					}
				}
				else if (*action_start)
				{
					/* Single-line IF statement - parse the action (e.g., "return $true") */
					action->argc = 1;
					action->args = safe_alloc(sizeof(char*));
					action->args[0] = strdup(action_start);
				}
			}
		}
		/* While loop: while ($condition) { actions } */
		else if (strncmp(current_line, "while (", 7) == 0)
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_WHILE;
			safe_strdup(action->function, "while");
			
			/* Parse the condition inside the parentheses */
			char *condition_start = current_line + 7; /* Skip "while (" */
			char *brace_pos = strchr(condition_start, '{');
			char *condition_end = NULL;
			
			if (brace_pos)
			{
				/* Search backwards from the brace to find the last ')' */
				char *search_pos = brace_pos - 1;
				while (search_pos > condition_start && *search_pos != ')')
				{
					search_pos--;
				}
				if (*search_pos == ')')
				{
					condition_end = search_pos;
				}
			}
			
			if (!condition_end)
			{
				condition_end = strchr(condition_start, ')');
			}
			
			if (condition_end)
			{
				int condition_len = condition_end - condition_start;
				char *condition_str = safe_alloc(condition_len + 1);
				strncpy(condition_str, condition_start, condition_len);
				condition_str[condition_len] = '\0';
				
				/* NEW: Parse as boolean expression to support || and && */
				action->bool_expr = parse_bool_expression(condition_str);
				
				/* Do NOT set action->condition to avoid double-free issues */
				/* The execute_script_action will check bool_expr first */
				action->condition = NULL;
				
				free(condition_str);
			}
			
			/* OLD CONDITION PARSING REMOVED - Using new bool_expr system */
			
			/* Add to action list */
			action->next = NULL;
			if (!actions)
				actions = action;
			else
				current_action->next = action;
			current_action = action;
			
			/* Set state for collecting nested actions - use LOOP state */
			/* Push current loop context onto stack if we're inside another loop */
			if (inside_loop_block && loop_depth < MAX_LOOP_DEPTH)
			{
				loop_stack[loop_depth].loop_action = current_loop_action;
				loop_stack[loop_depth].nested_head = loop_nested_head;
				loop_stack[loop_depth].nested_tail = loop_nested_tail;
				loop_stack[loop_depth].inside_loop = inside_loop_block;
				loop_depth++;
			}
			
			inside_loop_block = 1;
			current_loop_action = action;
			loop_nested_head = NULL;
			loop_nested_tail = NULL;
			
			current_line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}
		/* For loop: for (%var in start..end) { actions } or for (var %i = 1; %i != 10; %i++) */
		else if (strncmp(current_line, "for (", 5) == 0)
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_FOR;
			safe_strdup(action->function, "for");
			
			/* Parse: for (%var in start..end) or for (var %i = 1; %i != 10; %i++) */
			char *loop_spec = current_line + 5; /* Skip "for (" */
			char *paren_end = strchr(loop_spec, ')');
			
			if (paren_end)
			{
				int spec_len = paren_end - loop_spec;
				char *spec_str = safe_alloc(spec_len + 1);
				strncpy(spec_str, loop_spec, spec_len);
				spec_str[spec_len] = '\0';
				
				/* Check if this is a C-style for loop (has semicolons) */
				char *semicolon_pos = strchr(spec_str, ';');
				
				if (semicolon_pos)
				{
					/* C-style for loop: var %i = 1; %i != 10; %i++ */
					/* Parse initialization: var %i = 1 */
					char *init_end = semicolon_pos;
					int init_len = init_end - spec_str;
					action->loop_init = safe_alloc(init_len + 1);
					strncpy(action->loop_init, spec_str, init_len);
					action->loop_init[init_len] = '\0';
					
					/* Extract variable name from initialization */
					char *var_start = strstr(action->loop_init, "var ");
					if (var_start)
					{
						var_start += 4; /* Skip "var " */
						while (*var_start && isspace(*var_start)) var_start++;
						
						char *equals_pos = strchr(var_start, '=');
						if (equals_pos)
						{
							int var_len = equals_pos - var_start;
							char *var_name = safe_alloc(var_len + 1);
							strncpy(var_name, var_start, var_len);
							var_name[var_len] = '\0';
							
							/* Trim whitespace */
							char *var_trim = var_name;
							while (*var_trim && isspace(*var_trim)) var_trim++;
							char *var_end = var_trim + strlen(var_trim) - 1;
							while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
							
							action->loop_var = strdup(var_trim);
							free(var_name);
						}
					}
					
					/* Parse condition: %i != 10 */
					char *cond_start = semicolon_pos + 1;
					while (*cond_start && isspace(*cond_start)) cond_start++;
					
					char *cond_end = strchr(cond_start, ';');
					if (cond_end)
					{
						int cond_len = cond_end - cond_start;
						char *cond_str = safe_alloc(cond_len + 1);
						strncpy(cond_str, cond_start, cond_len);
						cond_str[cond_len] = '\0';
						
					/* Parse the condition */
					action->condition = safe_alloc(sizeof(ObbyScriptCondition));
					
					char *op_pos = strstr(cond_str, "!=");
					int is_not_equal = 1;
					if (!op_pos)
					{
						op_pos = strstr(cond_str, "==");
						is_not_equal = 0;
					}
					if (!op_pos)
					{
						op_pos = strstr(cond_str, "<=");
						is_not_equal = 4; /* <= operator */
					}
					if (!op_pos)
					{
						op_pos = strstr(cond_str, ">=");
						is_not_equal = 5; /* >= operator */
					}
					if (!op_pos)
					{
						op_pos = strstr(cond_str, "<");
						is_not_equal = 2; /* < operator */
					}
					if (!op_pos)
					{
						op_pos = strstr(cond_str, ">");
						is_not_equal = 3; /* > operator */
					}						if (op_pos)
						{
							int var_len = op_pos - cond_str;
							char *var_name = safe_alloc(var_len + 1);
							strncpy(var_name, cond_str, var_len);
							var_name[var_len] = '\0';
							
							char *var_trim = var_name;
							while (*var_trim && isspace(*var_trim)) var_trim++;
							char *var_end = var_trim + strlen(var_trim) - 1;
							while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
							
							action->condition->variable = strdup(var_trim);
							
							/* Set operator */
							if (is_not_equal == 0)
								action->condition->operator = strdup("==");
							else if (is_not_equal == 1)
								action->condition->operator = strdup("!=");
							else if (is_not_equal == 2)
								action->condition->operator = strdup("<");
							else if (is_not_equal == 3)
								action->condition->operator = strdup(">");
							else if (is_not_equal == 4)
								action->condition->operator = strdup("<=");
							else if (is_not_equal == 5)
								action->condition->operator = strdup(">=");
							
							/* Extract value */
							int op_len = (is_not_equal <= 1) ? 2 : 1;
							if (is_not_equal >= 4) op_len = 2;
							char *value_start = op_pos + op_len;
							while (*value_start && isspace(*value_start)) value_start++;
							
							char *value = strdup(value_start);
							char *value_end = value + strlen(value) - 1;
							while (value_end > value && isspace(*value_end)) *value_end-- = '\0';
							action->condition->value = value;
							
							free(var_name);
						}
						free(cond_str);
						
						/* Parse increment: %i++ or %i = %i + 1 */
						char *inc_start = cond_end + 1;
						while (*inc_start && isspace(*inc_start)) inc_start++;
						
						action->loop_increment = strdup(inc_start);
					}
				}
				else
				{
					/* Range-based for loop: %var in start..end */
					char *in_pos = strstr(spec_str, " in ");
					if (in_pos)
					{
						/* Extract variable name */
						int var_len = in_pos - spec_str;
						char *var_name = safe_alloc(var_len + 1);
						strncpy(var_name, spec_str, var_len);
						var_name[var_len] = '\0';
						
						/* Trim whitespace */
						char *var_trim = var_name;
						while (*var_trim && isspace(*var_trim)) var_trim++;
						char *var_end = var_trim + strlen(var_trim) - 1;
						while (var_end > var_trim && isspace(*var_end)) *var_end-- = '\0';
						
						action->loop_var = strdup(var_trim);
						
						/* Extract range: start..end */
						char *range_start = in_pos + 4;
						while (*range_start && isspace(*range_start)) range_start++;
						
						char *range_sep = strstr(range_start, "..");
						if (range_sep)
						{
							/* Parse start value */
							char *start_str = safe_alloc((range_sep - range_start) + 1);
							strncpy(start_str, range_start, range_sep - range_start);
							start_str[range_sep - range_start] = '\0';
							action->loop_start = atoi(start_str);
							free(start_str);
							
							/* Parse end value */
							char *end_str = range_sep + 2;
							while (*end_str && isspace(*end_str)) end_str++;
							action->loop_end = atoi(end_str);
							action->loop_step = 1; /* Default step */
						}
						free(var_name);
					}
				}
				free(spec_str);
				
				/* Add to action list */
				action->next = NULL;
				if (!actions)
					actions = action;
				else
					current_action->next = action;
				current_action = action;
				
				/* Set state for collecting nested actions - use LOOP state */
				/* Push current loop context onto stack before starting new loop */
				if (inside_loop_block && loop_depth < MAX_LOOP_DEPTH)
				{
					fprintf(stderr, "[PARSE_DEBUG] FOR loop inside another loop - pushing parent context and collecting FOR action\n");
					
					/* First, add this FOR action to the parent loop's nested list */
					if (!loop_nested_head)
					{
						loop_nested_head = loop_nested_tail = action;
					}
					else
					{
						loop_nested_tail->next = action;
						loop_nested_tail = action;
					}
					
					/* Now push the parent context (including this FOR action in nested list) */
					loop_stack[loop_depth].loop_action = current_loop_action;
					loop_stack[loop_depth].nested_head = loop_nested_head;
					loop_stack[loop_depth].nested_tail = loop_nested_tail;
					loop_stack[loop_depth].inside_loop = inside_loop_block;
					loop_depth++;
				}
				
				inside_loop_block = 1;
				current_loop_action = action;
				loop_nested_head = NULL;
				loop_nested_tail = NULL;
				
				current_line = strtok_r(NULL, "\n", &saveptr);
				continue;
			}
		}
		/* Generic command parsing - supports any IRC command */
		else if (isupper(current_line[0]) || strstr(current_line, " "))
		{
			action = safe_alloc(sizeof(ObbyScriptAction));
			action->type = US_ACTION_COMMAND;
			
			/* Parse command and arguments */
			char *cmd_copy = strdup(current_line);
			char *cmd_saveptr;
			char *command = strtok_r(cmd_copy, " ", &cmd_saveptr);
			
			if (command)
			{
				safe_strdup(action->function, command);
				
				/* Parse arguments */
				char *arg_tokens[20];
				int argc = 0;
				char *token;
				
				token = strtok_r(NULL, " ", &cmd_saveptr);
				while (token && argc < 20)
				{
					/* Handle quoted strings */
					if (token[0] == '"')
					{
						char *quoted_arg = safe_alloc(512);
						strcpy(quoted_arg, token + 1);  /* Skip opening quote */
						
						/* If the token doesn't end with quote, continue collecting */
						if (quoted_arg[strlen(quoted_arg)-1] != '"')
						{
							while ((token = strtok_r(NULL, " ", &cmd_saveptr)) != NULL)
							{
								strlcat(quoted_arg, " ", 512);
								strlcat(quoted_arg, token, 512);
								if (token[strlen(token)-1] == '"')
								{
									quoted_arg[strlen(quoted_arg)-1] = '\0'; /* Remove closing quote */
									break;
								}
							}
						}
						else
						{
							quoted_arg[strlen(quoted_arg)-1] = '\0'; /* Remove closing quote */
						}
						arg_tokens[argc++] = quoted_arg;
					}
					else
					{
						arg_tokens[argc++] = strdup(token);
					}
					token = strtok_r(NULL, " ", &cmd_saveptr);
				}
				
				action->argc = argc;
				if (argc > 0)
				{
					action->args = safe_alloc(sizeof(char*) * argc);
					for (int i = 0; i < argc; i++)
						action->args[i] = arg_tokens[i];
				}
			}
			
			free(cmd_copy);
		}
		
		if (action)
		{
			action->next = NULL;
			
			/* Priority: IF/ELSE > LOOP. If inside both, action belongs to the IF */
			if (inside_if_block || inside_else_block)
			{				
				/* Debug: Log nested action collection */
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_NESTED_ACTION_COLLECT", NULL,
					"Collecting nested action: function=$func, current_if=$if, in_else=$in_else",
					log_data_string("func", action->function ? action->function : "NULL"),
					log_data_string("if", current_if_action && current_if_action->function ? current_if_action->function : "NULL"),
					log_data_integer("in_else", inside_else_block));
				
				if (inside_else_block)
				{
					/* Collect actions for the ELSE branch */
					action->next = NULL; /* Ensure clean chain */
					if (!else_head)
					{
						else_head = else_tail = action;
					}
					else
					{
						else_tail->next = action;
						else_tail = action;
					}
				}
				else
				{
					/* Collect actions for the IF branch */
					action->next = NULL; /* Ensure clean chain */
					if (!nested_head)
					{
						nested_head = nested_tail = action;
					}
					else
					{
						nested_tail->next = action;
						nested_tail = action;
					}
					
					/* DEBUG: Log the chain structure with pointers */
					fprintf(stderr, "[CHAIN_DEBUG] Linked action: func=%s, action=%p, nested_head=%p, nested_tail=%p, tail->next=%p\n",
						action->function ? action->function : "NULL",
						action, nested_head, nested_tail, nested_tail ? nested_tail->next : NULL);
				}
			}
			else if (inside_loop_block)
			{
				fprintf(stderr, "[PARSE_DEBUG] Collecting action '%s' (type=%d) for LOOP body\n",
					action->function ? action->function : "NULL", action->type);
				/* Collect actions for the LOOP body (while/for) */
				if (!loop_nested_head)
				{
					loop_nested_head = loop_nested_tail = action;
				}
				else
				{
					loop_nested_tail->next = action;
					loop_nested_tail = action;
				}
			}
			else
			{
				/* Add to main actions list */
				if (!actions)
					actions = action;
				else
					current_action->next = action;
				current_action = action;
			}
		}

		current_line = strtok_r(NULL, "\n", &saveptr);
	}

	free(content_copy);
	return actions;
}

char *substitute_variables(const char *input, Client *client, Channel *channel)
{
	char *output;

	if (!input)
		return NULL;

	output = strdup(input);
	if (!output)
		return NULL;

	/* First, handle command parameter substitution if we're in a command context */
	if (current_command_parv && current_command_parc > 0)
	{
		char *temp_output = substitute_command_parameters(output, current_command_parc, current_command_parv, client, channel);
		if (temp_output)
		{
			free(output);
			output = temp_output;
		}
	}

	/* Handle function calls in the text */
	char *func_p = output;
	while ((func_p = strstr(func_p, "$")) != NULL)
	{
		/* Check if this might be a function call */
		char *paren = strchr(func_p, '(');
		if (paren)
		{
			/* Find the end of the function call */
			char *end_paren = strchr(paren, ')');
			if (end_paren)
			{
				/* Extract the function call */
				int call_len = end_paren - func_p + 1;
				char *func_call = safe_alloc(call_len + 1);
				strncpy(func_call, func_p, call_len);
				func_call[call_len] = '\0';
				
				/* Check if it's actually a function call */
				if (is_function_call(func_call))
				{
					/* Evaluate the function call */
					char *result = evaluate_condition_value(func_call, client, channel);
					if (result)
					{
						/* Replace the function call with its result */
						size_t prefix_len = func_p - output;
						size_t result_len = strlen(result);
						size_t suffix_len = strlen(end_paren + 1);
						char *new_output = safe_alloc(prefix_len + result_len + suffix_len + 1);
						
						strncpy(new_output, output, prefix_len);
						new_output[prefix_len] = '\0';
						strcat(new_output, result);
						strcat(new_output, end_paren + 1);
						
						free(output);
						output = new_output;
						free(result);
						
						/* Continue from after the replacement */
						func_p = new_output + prefix_len + result_len;
						free(func_call);
						continue;
					}
					free(result);
				}
				free(func_call);
			}
		}
		func_p++;
	}

	/* Validate variable syntax first */
	const char *p = input;
	while ((p = strchr(p, '$')) != NULL)
	{
		if (strncmp(p, "$true", 5) == 0)
		{
			/* $true is a valid boolean literal */
			if (p[5] == '\0' || isspace(p[5]) || p[5] == ')' || p[5] == ',' || p[5] == '"')
			{
				/* Valid boolean literal */
			}
			else
			{
				unreal_log(ULOG_ERROR, "obbyscript", "INVALID_VARIABLE", NULL,
					"Invalid variable syntax: Expected $true but found: $variable",
					log_data_string("variable", p));
				free(output);
				return strdup("SYNTAX_ERROR");
			}
		}
		else if (strncmp(p, "$false", 6) == 0)
		{
			/* $false is a valid boolean literal */
			if (p[6] == '\0' || isspace(p[6]) || p[6] == ')' || p[6] == ',' || p[6] == '"')
			{
				/* Valid boolean literal */
			}
			else
			{
				unreal_log(ULOG_ERROR, "obbyscript", "INVALID_VARIABLE", NULL,
					"Invalid variable syntax: Expected $false but found: $variable",
					log_data_string("variable", p));
				free(output);
				return strdup("SYNTAX_ERROR");
			}
		}
		else if (strncmp(p, "$client", 7) == 0)
		{
			/* Valid $client.* variables */
			if (p[7] == '.' || p[7] == '\0' || isspace(p[7]))
			{
				/* Valid syntax */
			}
			else
			{
				unreal_log(ULOG_ERROR, "obbyscript", "INVALID_VARIABLE", NULL,
					"Invalid variable syntax: Expected $client.property but found: $variable",
					log_data_string("variable", p));
				free(output);
				return strdup("SYNTAX_ERROR");
			}
		}
		else if (strncmp(p, "$chan", 5) == 0)
		{
			/* Check if it's $chan.name or just $chan */
			if (p[5] == '.' || p[5] == '\0' || isspace(p[5]))
			{
				/* $chan alone is valid for some contexts (sendnotice target)
				   $chan.name is valid for string contexts (mode command) */
			}
			else
			{
				unreal_log(ULOG_ERROR, "obbyscript", "INVALID_VARIABLE", NULL,
					"Invalid variable syntax: Expected $chan or $chan.property but found: $variable",
					log_data_string("variable", p));
				free(output);
				return strdup("SYNTAX_ERROR");
			}
		}
		else if (strncmp(p, "$channel", 8) == 0)
		{
			/* Check if it's $channel.name or just $channel */
			if (p[8] == '.' || p[8] == '\0' || isspace(p[8]))
			{
				/* Valid syntax */
			}
			else
			{
				unreal_log(ULOG_ERROR, "obbyscript", "INVALID_VARIABLE", NULL,
					"Invalid variable syntax: Expected $channel or $channel.property but found: $variable",
					log_data_string("variable", p));
				free(output);
				return strdup("SYNTAX_ERROR");
			}
		}
		else if (strncmp(p, "$server", 7) == 0)
		{
			/* Check if it's $server.name */
			if (p[7] == '.' || p[7] == '\0' || isspace(p[7]))
			{
				/* Valid syntax */
			}
			else
			{
				unreal_log(ULOG_ERROR, "obbyscript", "INVALID_VARIABLE", NULL,
					"Invalid variable syntax: Expected $server.property but found: $variable",
					log_data_string("variable", p));
				free(output);
				return strdup("SYNTAX_ERROR");
			}
		}
		p++;
	}

	/* Client variables */
	/* First check if $client is a function parameter (scope variable) */
	Client *effective_client = client;
	if (global_scope)
	{
		ObbyScriptVariable *client_var = get_variable_object(global_scope, "client");
		if (client_var && client_var->type == US_VAR_CLIENT && client_var->object_ptr)
		{
			effective_client = (Client *)client_var->object_ptr;
		}
	}
	
	if (effective_client)
	{
		if (strstr(output, "$client.name") && effective_client->name)
		{
			char *new_output = obbyscript_replace_string(output, "$client.name", effective_client->name);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$client.nick") && effective_client->name)
		{
			char *new_output = obbyscript_replace_string(output, "$client.nick", effective_client->name);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$client.ident") && effective_client->ident)
		{
			char *new_output = obbyscript_replace_string(output, "$client.ident", effective_client->ident);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$client.host") && effective_client->user && effective_client->user->realhost)
		{
			char *new_output = obbyscript_replace_string(output, "$client.host", effective_client->user->realhost);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$client.ip") && effective_client->ip)
		{
			char *new_output = obbyscript_replace_string(output, "$client.ip", effective_client->ip);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$client.gecos") && effective_client->info)
		{
			char *new_output = obbyscript_replace_string(output, "$client.gecos", effective_client->info);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$client.account") && effective_client->user && effective_client->user->account)
		{
			char *new_output = obbyscript_replace_string(output, "$client.account", effective_client->user->account);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$client.server") && effective_client->user && effective_client->user->server)
		{
			char *new_output = obbyscript_replace_string(output, "$client.server", effective_client->user->server);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$client.user.server") && effective_client->user && effective_client->user->server)
		{
			char *new_output = obbyscript_replace_string(output, "$client.user.server", effective_client->user->server);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		/* Generic $client replacement (should be last) */
		if (strstr(output, "$client") && !strstr(output, "$client.") && effective_client->name)
		{
			char *new_output = obbyscript_replace_string(output, "$client", effective_client->name);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
	}

	/* Channel variables */
	if (channel)
	{
		if (strstr(output, "$chan.name") && channel->name)
		{
			char *new_output = obbyscript_replace_string(output, "$chan.name", channel->name);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$channel.name") && channel->name)
		{
			char *new_output = obbyscript_replace_string(output, "$channel.name", channel->name);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$chan.topic") && channel->topic)
		{
			char *new_output = obbyscript_replace_string(output, "$chan.topic", channel->topic);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$channel.topic") && channel->topic)
		{
			char *new_output = obbyscript_replace_string(output, "$channel.topic", channel->topic);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$chan.users"))
		{
			char user_count[32];
			snprintf(user_count, sizeof(user_count), "%d", channel->users);
			char *new_output = obbyscript_replace_string(output, "$chan.users", user_count);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		if (strstr(output, "$channel.users"))
		{
			char user_count[32];
			snprintf(user_count, sizeof(user_count), "%d", channel->users);
			char *new_output = obbyscript_replace_string(output, "$channel.users", user_count);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		/* Generic $chan replacement (should be last) */
		if (strstr(output, "$chan") && !strstr(output, "$chan.") && channel->name)
		{
			char *new_output = obbyscript_replace_string(output, "$chan", channel->name);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
		
		/* Generic $channel replacement (should be last) */
		if (strstr(output, "$channel") && !strstr(output, "$channel.") && channel->name)
		{
			char *new_output = obbyscript_replace_string(output, "$channel", channel->name);
			if (new_output) {
				free(output);
				output = new_output;
			}
		}
	}
	
	/* Server variables */
	if (strstr(output, "$server.name"))
	{
		char *new_output = obbyscript_replace_string(output, "$server.name", me.name);
		if (new_output) {
			free(output);
			output = new_output;
		}
	}
	
	/* Time variables */
	if (strstr(output, "$time"))
	{
		char timestr[64];
		time_t now = time(NULL);
		snprintf(timestr, sizeof(timestr), "%ld", now);
		char *new_output = obbyscript_replace_string(output, "$time", timestr);
		if (new_output) {
			free(output);
			output = new_output;
		}
	}
	
	/* Built-in boolean variables - preserve as literals for display */
	/* $true and $false are handled in is_truthy_value() for conditional evaluation */
	/* $null is left as-is for proper comparison in conditions */
	
	/* Script variables - substitute %varname with variable values */
	if (global_scope)
	{
		char *p = output;
		while ((p = strchr(p, '%')) != NULL)
		{
		/* Find the end of the variable name */
		char *var_start = p + 1;
		char *var_end = var_start;
		while (*var_end && (isalnum(*var_end) || *var_end == '_')) var_end++;
		
		/* Save the end of the actual variable name before parsing array/property accessors */
		char *var_name_end = var_end;
		
		/* Check if there's an array index accessor [index] */
		char *array_index_str = NULL;
		if (*var_end == '[')
		{
			char *index_start = var_end + 1;
			char *index_end = strchr(index_start, ']');
			if (index_end)
			{
				int index_len = index_end - index_start;
				array_index_str = safe_alloc(index_len + 1);
				strncpy(array_index_str, index_start, index_len);
				array_index_str[index_len] = '\0';
				var_end = index_end + 1; /* Update var_end to skip past ] */
			}
		}
		
		/* Check if there's a property accessor (.property) */
		char *property_name = NULL;
		if (*var_end == '.')
		{
			char *prop_start = var_end + 1;
			char *prop_end = prop_start;
			while (*prop_end && (isalnum(*prop_end) || *prop_end == '_')) prop_end++;
			
			if (prop_end > prop_start)
			{
				int prop_len = prop_end - prop_start;
				property_name = safe_alloc(prop_len + 1);
				strncpy(property_name, prop_start, prop_len);
				property_name[prop_len] = '\0';
				var_end = prop_end; /* Update var_end to include property */
			}
		}
		
		if (var_name_end > var_start)
		{
			/* Extract variable name (just the name, not including [index] or .property) */
			int var_len = var_name_end - var_start;
			char *var_name = safe_alloc(var_len + 1);
			strncpy(var_name, var_start, var_len);
			var_name[var_len] = '\0';
			
			/* Get variable */
			ObbyScriptVariable *var = get_variable_object(global_scope, var_name);
				
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_VAR_LOOKUP", NULL,
					"Looking up variable '$var_name' with property '$property': found=$found",
					log_data_string("var_name", var_name),
					log_data_string("property", property_name ? property_name : "none"),
					log_data_integer("found", var ? 1 : 0));
				
				if (var)
				{
					char *replacement_value = NULL;
					
					/* Handle array index access */
					if (array_index_str && var->type == US_VAR_ARRAY && var->array_ptr)
					{
						/* Evaluate the index (could be a variable or literal) */
						char *index_value = substitute_variables(array_index_str, client, channel);
						
						if (index_value && *index_value)
						{
							int index = atoi(index_value);
							free(index_value);
							
							/* Get array element */
							char *elem_str = array_get_string(var->array_ptr, index);
							
							if (elem_str)
							{
								replacement_value = strdup(elem_str);
							}
							else
							{
								/* Index out of bounds or element is null */
								replacement_value = strdup("$null");
							}
						}
						else
						{
							/* Failed to evaluate index */
							if (index_value) free(index_value);
							replacement_value = strdup("$null");
						}
					}
					else if (property_name && var->object_ptr)
					{
						/* Handle object property access */
						if (var->type == US_VAR_CLIENT)
						{
							Client *var_client = (Client *)var->object_ptr;
							if (strcmp(property_name, "name") == 0 && var_client->name)
								replacement_value = strdup(var_client->name);
							else if (strcmp(property_name, "host") == 0 && var_client->user && var_client->user->realhost)
								replacement_value = strdup(var_client->user->realhost);
							else if (strcmp(property_name, "ip") == 0 && var_client->ip)
								replacement_value = strdup(var_client->ip);
							else if (strcmp(property_name, "server") == 0 && var_client->user && var_client->user->server)
								replacement_value = strdup(var_client->user->server);
							else if (strcmp(property_name, "account") == 0 && var_client->user && var_client->user->account)
								replacement_value = strdup(var_client->user->account);
						}
						else if (var->type == US_VAR_CHANNEL)
						{
							Channel *var_channel = (Channel *)var->object_ptr;
							if (strcmp(property_name, "name") == 0 && var_channel->name)
								replacement_value = strdup(var_channel->name);
							else if (strcmp(property_name, "topic") == 0 && var_channel->topic)
								replacement_value = strdup(var_channel->topic);
							else if (strcmp(property_name, "users") == 0)
							{
								char *users_count = safe_alloc(16);
								snprintf(users_count, 16, "%d", var_channel->users);
								replacement_value = users_count;
							}
						}
					}
					else if (property_name && var->type == US_VAR_STRING && var->value)
					{
						/* Handle STRING variables with property accessors - just return the base value */
						/* This handles cases like %chan_poo.name when %chan_poo is "$false" */
						replacement_value = strdup(var->value);
					}
					else if (!property_name && var->value)
					{
						/* Handle simple variable access */
						replacement_value = strdup(var->value);
					}
					
					if (replacement_value)
					{
						/* Log the resolved value for debugging purposes */
						unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_VAR_VALUE", NULL,
							"Variable $var_name property $property resolved to '$value'",
							log_data_string("var_name", var_name),
							log_data_string("property", property_name ? property_name : "none"),
							log_data_string("value", replacement_value));

						/* Build replacement string with % prefix and property if present */
						int full_var_len = var_end - p;
						char *var_with_percent = safe_alloc(full_var_len + 1);
						strncpy(var_with_percent, p, full_var_len);
						var_with_percent[full_var_len] = '\0';
						
						/* Replace in output */
						char *new_output = obbyscript_replace_string(output, var_with_percent, replacement_value);
						if (new_output)
						{
							free(output);
							output = new_output;
							p = output; /* Start over since string changed */
						}
						else
						{
							p = var_end; /* Move past this variable */
						}
						
						free(var_with_percent);
						free(replacement_value);
					}
					else
					{
						p = var_end; /* Move past this variable */
					}
				}
				else
				{
					/* Try simple variable lookup for backward compatibility */
					char *var_value = get_variable(global_scope, var_name);
					if (var_value && !property_name)
					{
						/* Build replacement string with % prefix */
						char *var_with_percent = safe_alloc(var_len + 2);
						snprintf(var_with_percent, var_len + 2, "%%%s", var_name);
						
						/* Replace in output */
						char *new_output = obbyscript_replace_string(output, var_with_percent, var_value);
						if (new_output)
						{
							free(output);
							output = new_output;
							p = output; /* Start over since string changed */
						}
						else
						{
							p = var_end; /* Move past this variable */
						}
						
						free(var_with_percent);
					}
					else
					{
						p = var_end; /* Move past this variable */
					}
				}
				
				free(var_name);
				if (property_name) free(property_name);
				if (array_index_str) free(array_index_str);
			}
			else
			{
				p++; /* Skip this % */
				if (property_name) free(property_name);
			}
		}
	}

	return output;
}

void execute_script_action(ObbyScriptAction *action, Client *client, Channel *channel)
{
	if (!action)
		return;



	/* Some actions require a client, others don't */
	if (!client && (action->type == US_ACTION_COMMAND || action->type == US_ACTION_SENDNOTICE || action->type == US_ACTION_IF))
	{

		return;
	}

	switch (action->type)
	{
		case US_ACTION_COMMAND:
		{
			/* Execute generic IRC command via do_cmd */
			if (action->function && action->argc >= 0)
			{
				const char *parv[25];  /* IRC command argument array */
				int parc = 0;
				
				/* Set up parv array - parv[0] should be NULL */
				parv[parc++] = NULL;  /* parv[0] is always NULL */
				
				/* Process arguments with variable substitution */
				for (int i = 0; i < action->argc && parc < 24; i++)
				{
					char *substituted = substitute_variables(action->args[i], client, channel);
					if (substituted)
					{
						/* Check for syntax errors */
						if (strcmp(substituted, "SYNTAX_ERROR") == 0)
						{
							unreal_log(ULOG_ERROR, "obbyscript", "COMMAND_SYNTAX_ERROR", NULL,
								"Syntax error in command '$command' argument '$arg' - script execution aborted",
								log_data_string("command", action->function),
								log_data_string("arg", action->args[i]));
							
							/* Free any previously allocated arguments */
							for (int j = 1; j < parc; j++)
							{
								if (parv[j])
								{
									free((char*)parv[j]);
								}
							}
							free(substituted);
							return; /* Abort execution */
						}
						
						parv[parc++] = substituted;
					}
					else
					{
						/* substitute_variables failed - use original string but don't mark for freeing */
						parv[parc++] = action->args[i];
					}
				}
				parv[parc] = NULL;  /* Last element is NULL */
				
				/* Check if this is a destructive command that should be deferred */
				if (is_destructive_command(action->function))
				{
					/* Defer execution to avoid freeing memory during hook processing */
					add_deferred_action(action->function, &parv[1], parc - 1, client, channel);
				}
				else
				{
					/* Execute the command immediately */
					do_cmd(&me, NULL, action->function, parc, parv);
				}
				
				/* 
				 * DO NOT FREE ARGUMENTS AFTER do_cmd()!
				 * 
				 * The do_cmd() function may modify the parv array to point to internal
				 * UnrealIRCd buffers, and freeing these would cause a crash.
				 * This creates a small memory leak, but it's safer than crashing.
				 * 
				 * The memory will be freed when the module unloads anyway.
				 */
			}
			break;
		}

		case US_ACTION_SENDNOTICE:
		{
			/* Legacy sendnotice - convert to NOTICE command */
			if (action->argc >= 2)
			{
				Client *target = NULL;
				char *message;
				char *target_name = substitute_variables(action->args[0], client, channel);

				/* Look up target client */
				if (target_name)
				{
					target = find_user(target_name, NULL);
					if (!target)
					{
						/* Try to find by nickname using find_client */
						target = find_client(target_name, NULL);
					}
					free(target_name);
				}

				/* If no target found, default to the triggering client */
				if (!target)
					target = client;

				if (action->argc >= 3)
				{
					message = substitute_variables(action->args[2], client, channel);
				}
				else
				{
					message = substitute_variables(action->args[1], client, channel);
				}

				if (message)
				{
					/* Use do_cmd to send NOTICE */
					const char *parv[4];
					parv[0] = NULL;  /* parv[0] should be NULL */
					parv[1] = target->name;  /* Target */
					parv[2] = message;  /* Message */
					parv[3] = NULL;  /* Last element NULL */
					
					do_cmd(&me, NULL, "NOTICE", 3, parv);
					
					free(message);
				}
			}
			break;
		}

		case US_ACTION_IF:
		{
			int condition_result = 0;
			
			/* NEW: Check bool_expr first, fall back to legacy condition */
			if (action->bool_expr)
			{
				condition_result = evaluate_bool_expr(action->bool_expr, client, channel);
			}
			else if (action->condition)
			{
				unreal_log(ULOG_DEBUG, "obbyscript", "MAIN_CONDITION_EXECUTE_DEBUG", NULL,
					"Main execution: About to evaluate condition - variable: '$variable', operator: '$operator', value: '$value'",
					log_data_string("variable", action->condition->variable ? action->condition->variable : "NULL"),
					log_data_string("operator", action->condition->operator ? action->condition->operator : "NULL"),
					log_data_string("value", action->condition->value ? action->condition->value : "NULL"));
				
			condition_result = evaluate_condition(action->condition, client, channel);
		}
		
		unreal_log(ULOG_INFO, "obbyscript", "IF_CONDITION_RESULT", NULL,
			"IF statement: condition_result = $result",
			log_data_integer("result", condition_result));
		
		if (condition_result)
		{
			unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_IF_TRUE", NULL,
				"IF condition evaluated to TRUE, checking for nested actions");				/* Debug: Check what we have */
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_IF_PATHS", NULL,
					"IF debug: argc=$argc, args0=$args0, has_nested=$has_nested",
					log_data_integer("argc", action->argc),
					log_data_string("args0", action->argc > 0 && action->args[0] ? action->args[0] : "NULL"),
					log_data_integer("has_nested", action->nested_actions ? 1 : 0));
				
				/* Handle single-line if statement with action */
				if (action->argc > 0 && action->args[0])
				{
					unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_IF_SINGLE_LINE", NULL,
						"Executing single-line IF action: $action",
						log_data_string("action", action->args[0]));
						
					/* Parse and execute the action (e.g., "return $true") */
					char *action_str = action->args[0];
					
					if (strncmp(action_str, "return ", 7) == 0)
					{
						/* Handle return statement */
						char *return_value = action_str + 7;
						while (*return_value && isspace(*return_value)) return_value++;
						
						/* This should be handled in the CAN_JOIN hook return value */
						/* For now, we'll set a special variable to indicate the return value */
						if (strcmp(return_value, "$true") == 0)
						{
							set_variable(global_scope, "__return", "true", 0);
						}
						else if (strcmp(return_value, "$false") == 0)
						{
							set_variable(global_scope, "__return", "false", 0);
						}
					}
					else
					{
						/* Execute other actions like sendnotice, etc. */
						/* Create a temporary action structure */
						ObbyScriptAction *temp_action = safe_alloc(sizeof(ObbyScriptAction));
						
						/* Parse the action string */
						if (strncmp(action_str, "sendnotice ", 11) == 0)
						{
							temp_action->type = US_ACTION_SENDNOTICE;
							safe_strdup(temp_action->function, "sendnotice");
							
							char *notice_args = action_str + 11;
							temp_action->argc = 1;
							temp_action->args = safe_alloc(sizeof(char*));
							temp_action->args[0] = strdup(notice_args);
							
							execute_script_action(temp_action, client, channel);
						}
						
						free_script_action(temp_action);
					}
				}
				else if (action->nested_actions)
				{
					unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_IF_NESTED", NULL,
						"Executing nested IF actions");
						
					/* Handle multi-line if blocks - condition is TRUE */
					/* Execute each nested action in sequence */
					ObbyScriptAction *current_nested = action->nested_actions;
					int action_count = 0;
					while (current_nested)
					{
						action_count++;
						fprintf(stderr, "[EXEC_DEBUG] Executing nested action #%d: func=%s, ptr=%p, has_next=%d, next_ptr=%p\n",
							action_count,
							current_nested->function ? current_nested->function : "NULL",
							current_nested,
							current_nested->next ? 1 : 0,
							current_nested->next);
						
						/* IMPORTANT: Save and clear the next pointer to prevent recursive execution */
						ObbyScriptAction *saved_next = current_nested->next;
						current_nested->next = NULL;
						
						if (client && channel)
						{
							execute_script_action(current_nested, client, channel);
						}
						else if (client)
						{
							execute_script_action(current_nested, client, NULL);
						}
						else
						{
							unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_IF_NESTED_NOCLIENT", NULL,
								"Cannot execute nested action - no client context");
						}
						
						/* Restore the next pointer */
						current_nested->next = saved_next;
						
						/* Check if a return statement was executed */
						char *return_val = get_variable(global_scope, "__return__");
						if (return_val)
						{
							/* A return was executed, stop further execution */
							return;
						}
						
						/* Check if break or continue was executed */
						if (should_break || should_continue)
						{
							unreal_log(ULOG_INFO, "obbyscript", "IF_BREAK_CONTINUE", NULL,
								"IF nested action set break/continue flag - stopping nested execution");
							/* Stop executing nested actions, let the flag propagate to the loop */
							break;
						}
						
						current_nested = saved_next;
					}
				}
				
				/* CRITICAL: Break here to prevent else execution */
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_CONDITIONAL_IF_DONE", NULL,
					"IF branch completed - should NOT execute else");
				/* Continue to next action after IF/ELSE is complete */
				if (action->next)
				{
					unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_CONDITIONAL_CONTINUE", NULL,
						"Continuing to next action after IF execution");
					execute_script_action(action->next, client, channel);
				}
				return; /* Exit function, don't continue normal flow */
			}
			else if (action->else_actions)
			{
				/* Handle else block - condition is FALSE */
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_CONDITIONAL_ELSE", NULL,
					"Executing else block - condition was false");
				
				/* Execute each else action in sequence, preventing recursive next-action calls */
				ObbyScriptAction *current_else = action->else_actions;
				while (current_else)
				{
					/* Save and clear the next pointer to prevent recursive execution */
					ObbyScriptAction *saved_next = current_else->next;
					current_else->next = NULL;
					
					if (client && channel)
					{
						execute_script_action(current_else, client, channel);
					}
					else if (client)
					{
						execute_script_action(current_else, client, NULL);
					}
					else
					{
						unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_CONDITIONAL_ELSE_NOCLIENT", NULL,
							"Cannot execute else - no client context");
					}
					
					/* Restore the next pointer */
					current_else->next = saved_next;
					
					/* Check if a return statement was executed inside the else block */
					char *return_val = get_variable(global_scope, "__return__");
					if (return_val)
					{
						/* A return was executed, stop further execution */
						return;
					}
					
					current_else = saved_next;
				}
				
				/* Break here too */
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_CONDITIONAL_ELSE_DONE", NULL,
					"ELSE branch completed");
				/* Continue to next action after IF/ELSE is complete */
				if (action->next)
				{
					unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_CONDITIONAL_CONTINUE", NULL,
						"Continuing to next action after ELSE execution");
					execute_script_action(action->next, client, channel);
				}
				return; /* Exit function, don't continue normal flow */
			}
			else
			{
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_CONDITIONAL_NOELSE", NULL,
					"No else block to execute - condition was false");
				/* Continue to next action even when no else block */
				if (action->next)
				{
					unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_CONDITIONAL_CONTINUE", NULL,
						"Continuing to next action after IF (no else)");
					execute_script_action(action->next, client, channel);
				}
				return; /* Exit function, don't continue normal flow */
			}
			break;
		}

		case US_ACTION_WHILE:
		{
			fprintf(stderr, "[EXEC_DEBUG] === WHILE LOOP EXECUTION STARTING ===\n");
			fprintf(stderr, "[EXEC_DEBUG] has_nested_actions = %d\n", action->nested_actions ? 1 : 0);
			
			/* Execute while loop */
			int loop_safety_counter = 0;
			const int MAX_LOOP_ITERATIONS = 10000; /* Safety limit to prevent infinite loops */
			
			unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_WHILE_START", NULL,
				"Starting WHILE loop execution, has_nested=$has_nested",
				log_data_integer("has_nested", action->nested_actions ? 1 : 0));
			
			while (loop_safety_counter < MAX_LOOP_ITERATIONS)
			{
				/* Evaluate the loop condition */
				int condition_result = 0;
				
				/* NEW: Check bool_expr first, fall back to legacy condition */
				if (action->bool_expr)
				{
					condition_result = evaluate_bool_expr(action->bool_expr, client, channel);
				}
				else if (action->condition)
				{
					condition_result = evaluate_condition(action->condition, client, channel);
				}
				
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_WHILE_CONDITION", NULL,
					"WHILE loop iteration $iter, condition=$cond",
					log_data_integer("iter", loop_safety_counter),
					log_data_integer("cond", condition_result));
				
				if (!condition_result)
				{
					/* Condition is false, exit the loop */
					break;
				}
				
				/* Execute nested actions */
				should_break = 0;
				should_continue = 0;
				
				if (action->nested_actions)
				{
					ObbyScriptAction *current_nested = action->nested_actions;
					int action_count = 0;
					while (current_nested)
					{
						action_count++;
						unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_WHILE_ACTION", NULL,
							"Executing WHILE nested action #$num: $func ($type)",
							log_data_integer("num", action_count),
							log_data_string("func", current_nested->function ? current_nested->function : "NULL"),
							log_data_integer("type", current_nested->type));
						
						ObbyScriptAction *saved_next = current_nested->next;
						current_nested->next = NULL;
						
						/* Check for break/continue */
						if (current_nested->type == US_ACTION_BREAK)
						{
							should_break = 1;
							current_nested->next = saved_next;
							break;
						}
						else if (current_nested->type == US_ACTION_CONTINUE)
						{
							should_continue = 1;
							current_nested->next = saved_next;
							break;
						}
						
						if (client && channel)
						{
							execute_script_action(current_nested, client, channel);
						}
						else if (client)
						{
							execute_script_action(current_nested, client, NULL);
						}
						
						current_nested->next = saved_next;
						
						/* Check for return statement */
						char *return_val = get_variable(global_scope, "__return__");
						if (return_val)
						{
							return;
						}
						
						current_nested = saved_next;
					}
				}
				
				/* Handle break - exit the loop entirely */
				if (should_break)
				{
					break;
				}
				
				/* Handle continue - go to next iteration */
				if (should_continue)
				{
					loop_safety_counter++;
					continue;
				}
				
				loop_safety_counter++;
			}
			

			if (loop_safety_counter >= MAX_LOOP_ITERATIONS)
			{
				unreal_log(ULOG_WARNING, "obbyscript", "WHILE_LOOP_LIMIT", NULL,
					"While loop reached maximum iteration limit ($limit) - possible infinite loop",
					log_data_integer("limit", MAX_LOOP_ITERATIONS));
			}
			
			/* Loop completed, continue to next action via normal flow */
			break;
		}

		case US_ACTION_FOR:
		{
			fprintf(stderr, "[EXEC_DEBUG] === FOR LOOP EXECUTION STARTING ===\n");
			fprintf(stderr, "[EXEC_DEBUG] has_nested_actions = %d\n", action->nested_actions ? 1 : 0);
			fprintf(stderr, "[EXEC_DEBUG] has_loop_init = %d\n", action->loop_init ? 1 : 0);
			
			/* Execute for loop */
			if (action->loop_init)
			{
				/* C-style for loop: for (var %i = 1; %i != 10; %i++) */
				
				/* Execute initialization */
				if (strncmp(action->loop_init, "var ", 4) == 0)
				{
					char *init_copy = strdup(action->loop_init + 4); /* Skip "var " */
					char *equals = strchr(init_copy, '=');
					
					if (equals)
					{
						/* Format: var %i = 1 */
						*equals = '\0';
						char *var_name = init_copy;
						char *value_str = equals + 1;
						
						/* Trim whitespace */
						while (*var_name && isspace(*var_name)) var_name++;
						char *var_end = var_name + strlen(var_name) - 1;
						while (var_end > var_name && isspace(*var_end)) *var_end-- = '\0';
						
						while (*value_str && isspace(*value_str)) value_str++;
						char *val_end = value_str + strlen(value_str) - 1;
						while (val_end > value_str && isspace(*val_end)) *val_end-- = '\0';
						
						/* Set initial value */
						set_variable(global_scope, var_name, value_str, 0);
					}
					else
					{
						/* Format: var %i 1 (space-separated, no equals) */
						char *var_name = init_copy;
						
						/* Find first space after variable name */
						while (*var_name && isspace(*var_name)) var_name++;
						char *space = var_name;
						while (*space && !isspace(*space)) space++;
						
						if (*space)
						{
							*space = '\0';
							char *value_str = space + 1;
							while (*value_str && isspace(*value_str)) value_str++;
							
							/* Trim trailing whitespace from value */
							char *val_end = value_str + strlen(value_str) - 1;
							while (val_end > value_str && isspace(*val_end)) *val_end-- = '\0';
							
							/* Set initial value */
							set_variable(global_scope, var_name, value_str, 0);
						}
					}
					free(init_copy);
				}
				
				/* Loop until condition is false */
				int loop_safety_counter = 0;
				const int MAX_LOOP_ITERATIONS = 10000;
				
				while (loop_safety_counter < MAX_LOOP_ITERATIONS)
				{
					/* Evaluate condition */
					int condition_result = 0;
					if (action->condition)
					{
						condition_result = evaluate_condition(action->condition, client, channel);
					}
					
					if (!condition_result)
					{
						break;
					}
					
					/* Execute nested actions */
					should_break = 0;
					should_continue = 0;
					
					if (action->nested_actions)
					{
						ObbyScriptAction *current_nested = action->nested_actions;
						while (current_nested)
						{
							ObbyScriptAction *saved_next = current_nested->next;
							current_nested->next = NULL;
							
							unreal_log(ULOG_INFO, "obbyscript", "FOR_NESTED_ACTION", NULL,
								"FOR loop executing nested action type=$type",
								log_data_integer("type", current_nested->type));
							
							/* Check for break/continue */
							if (current_nested->type == US_ACTION_BREAK)
							{
								should_break = 1;
								current_nested->next = saved_next;
								break;
							}
							else if (current_nested->type == US_ACTION_CONTINUE)
							{
								should_continue = 1;
								current_nested->next = saved_next;
								break;
							}
							
							if (client && channel)
							{
								execute_script_action(current_nested, client, channel);
							}
							else if (client)
							{
								execute_script_action(current_nested, client, NULL);
							}
							
							current_nested->next = saved_next;
							
							/* Check for return statement */
							char *return_val = get_variable(global_scope, "__return__");
							if (return_val)
							{
								return;
							}
							
							current_nested = saved_next;
						}
					}
					
					/* Handle break - exit the loop entirely */
					if (should_break)
					{
						break;
					}
					
					/* Handle continue - skip increment and go to next iteration */
					if (should_continue)
					{
						/* Skip increment, continue to next iteration */
						loop_safety_counter++;
						continue;
					}
					
					/* Execute increment */
					if (action->loop_increment)
					{
						char *inc = action->loop_increment;
						
						/* Handle %i++ or %i = %i + 1 */
						if (strstr(inc, "++"))
						{
							/* Extract variable name */
							char *var_name = strdup(inc);
							char *plus_plus = strstr(var_name, "++");
							if (plus_plus)
							{
								*plus_plus = '\0';
								/* Trim whitespace */
								char *trim = var_name;
								while (*trim && isspace(*trim)) trim++;
								char *end = trim + strlen(trim) - 1;
								while (end > trim && isspace(*end)) *end-- = '\0';
								
								/* Get current value and increment */
								char *current_val = get_variable(global_scope, trim);
								if (current_val)
								{
									int new_val = atoi(current_val) + 1;
									char new_str[32];
									snprintf(new_str, sizeof(new_str), "%d", new_val);
									set_variable(global_scope, trim, new_str, 0);
								}
							}
							free(var_name);
						}
						else if (strstr(inc, "--"))
						{
							/* Extract variable name */
							char *var_name = strdup(inc);
							char *minus_minus = strstr(var_name, "--");
							if (minus_minus)
							{
								*minus_minus = '\0';
								/* Trim whitespace */
								char *trim = var_name;
								while (*trim && isspace(*trim)) trim++;
								char *end = trim + strlen(trim) - 1;
								while (end > trim && isspace(*end)) *end-- = '\0';
								
								/* Get current value and decrement */
								char *current_val = get_variable(global_scope, trim);
								if (current_val)
								{
									int new_val = atoi(current_val) - 1;
									char new_str[32];
									snprintf(new_str, sizeof(new_str), "%d", new_val);
									set_variable(global_scope, trim, new_str, 0);
								}
							}
							free(var_name);
						}
						else if (strchr(inc, '='))
						{
							/* Handle %i = %i + 1 or %i = %i - 1 */
							char *inc_copy = strdup(inc);
							char *equals = strchr(inc_copy, '=');
							if (equals)
							{
								*equals = '\0';
								char *var_name = inc_copy;
								char *expression = equals + 1;
								
								/* Trim whitespace */
								while (*var_name && isspace(*var_name)) var_name++;
								char *var_end = var_name + strlen(var_name) - 1;
								while (var_end > var_name && isspace(*var_end)) *var_end-- = '\0';
								
								while (*expression && isspace(*expression)) expression++;
								
								/* Evaluate arithmetic expression */
								int result = evaluate_arithmetic(expression, client, channel);
								char result_str[32];
								snprintf(result_str, sizeof(result_str), "%d", result);
								set_variable(global_scope, var_name, result_str, 0);
							}
							free(inc_copy);
						}
					}
					
					loop_safety_counter++;
				}
				
				if (loop_safety_counter >= MAX_LOOP_ITERATIONS)
				{
					unreal_log(ULOG_WARNING, "obbyscript", "FOR_LOOP_LIMIT", NULL,
						"C-style for loop reached maximum iteration limit ($limit) - possible infinite loop",
						log_data_integer("limit", MAX_LOOP_ITERATIONS));
				}
			}
			else if (action->loop_var)
			{
				/* Range-based for loop: for (%i in 1..10) */
				for (int i = action->loop_start; i <= action->loop_end; i += action->loop_step)
				{
					/* Set the loop variable */
					char loop_value[32];
					snprintf(loop_value, sizeof(loop_value), "%d", i);
					set_variable(global_scope, action->loop_var, loop_value, 0);
					
					/* Execute nested actions */
					if (action->nested_actions)
					{
						ObbyScriptAction *current_nested = action->nested_actions;
						while (current_nested)
						{
							ObbyScriptAction *saved_next = current_nested->next;
							current_nested->next = NULL;
							
							if (client && channel)
							{
								execute_script_action(current_nested, client, channel);
							}
							else if (client)
							{
								execute_script_action(current_nested, client, NULL);
							}
							
							current_nested->next = saved_next;
							
							/* Check for return statement */
							char *return_val = get_variable(global_scope, "__return__");
							if (return_val)
							{
								return;
							}
							
							current_nested = saved_next;
						}
					}
				}
			}
			
			/* Loop completed, continue to next action via normal flow */
			break;
		}

		case US_ACTION_VAR:
		{
			/* Handle variable declaration/assignment */
			unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_VAR_PARSE", NULL,
				"Variable action parsed with $argc args",
				log_data_integer("argc", action->argc));
			
			for (int i = 0; i < action->argc; i++)
			{
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_VAR_ARG", NULL,
					"args[$i] = '$arg'",
					log_data_integer("i", i),
					log_data_string("arg", action->args[i]));
			}
			
			if (action->argc >= 4 && strcmp(action->args[0], "var") == 0 && strcmp(action->args[2], "=") == 0)
			{
				/* Format: var %name = value */
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_VAR_ASSIGN", NULL,
					"Variable assignment: $var_name = $value",
					log_data_string("var_name", action->args[1]),
					log_data_string("value", action->args[3]));
				
				/* Check if value is an array literal */
				if (action->args[3] && action->args[3][0] == '[')
				{
					/* Parse and create array */
					ObbyScriptArray *array = parse_array_literal(action->args[3], client, channel);
					
					if (array)
					{
						set_variable_array(global_scope, action->args[1], array, 0);
					}
					break;
				}
				/* Check if value is $client.channels */
				else if (action->args[3] && strcmp(action->args[3], "$client.channels") == 0 && client)
				{
					ObbyScriptArray *array = get_client_channels(client);
					if (array)
					{
						set_variable_array(global_scope, action->args[1], array, 0);
					}
					break;
				}
				/* Check if value is a function call */
				else if (action->args[3] && is_function_call(action->args[3]))
				{
					unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_VAR_FUNC_CALL", NULL,
						"Detected function call: $func_call",
						log_data_string("func_call", action->args[3]));
					
					/* Parse function call: $function_name(args...) or function_name(args...) */
					char *func_call = strdup(action->args[3]);
					char *func_name = func_call;
					
					/* Skip $ if present for user-defined functions */
					if (*func_name == '$')
						func_name++;
					
					char *args_start = strchr(func_name, '(');
					if (args_start)
					{
						*args_start = '\0'; /* Terminate function name */
						args_start++; /* Move to first argument */
						
						/* Find the closing parenthesis */
						char *args_end = strrchr(args_start, ')');
						if (args_end)
						{
							*args_end = '\0'; /* Terminate arguments */
							
							/* Parse arguments */
							char **func_args = NULL;
							int func_argc = 0;
							
							if (strlen(args_start) > 0)
							{
								/* Count commas to determine argument count */
								func_argc = 1;
								for (char *p = args_start; *p; p++)
								{
									if (*p == ',') func_argc++;
								}
								
								func_args = safe_alloc(sizeof(char*) * func_argc);
								char *arg_copy = strdup(args_start);
								char *saveptr;
								char *token = strtok_r(arg_copy, ",", &saveptr);
								int i = 0;
								
								while (token && i < func_argc)
								{
									/* Trim whitespace and quotes */
									while (*token && isspace(*token)) token++;
									if (*token == '"' && token[strlen(token)-1] == '"')
									{
										token++;
										token[strlen(token)-1] = '\0';
									}
									func_args[i] = substitute_variables(token, client, channel);
									if (!func_args[i])
										func_args[i] = strdup(token);
									i++;
									token = strtok_r(NULL, ",", &saveptr);
								}
								free(arg_copy);
							}
							
							/* Execute function and get return value */
							if (is_builtin_function(func_name))
							{
								unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_BUILTIN_CALL", NULL,
									"Executing built-in function: $func_name with $argc args",
									log_data_string("func_name", func_name),
									log_data_integer("argc", func_argc));
								
								ObbyScriptVariable *result = execute_builtin_function(func_name, func_args, func_argc);
								if (result)
								{
									unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_BUILTIN_RESULT", NULL,
										"Built-in function returned object of type $type, storing in variable $var_name",
										log_data_integer("type", result->type),
										log_data_string("var_name", action->args[1]));
									
									/* Check if result is a STRING (indicates not found - $false) */
									if (result->type == US_VAR_STRING && result->value)
									{
										/* Store the string value (e.g., "$false") */
										set_variable(global_scope, action->args[1], result->value, 0);
										free(result->name);
										if (result->value) free(result->value);
										free(result);
									}
									else
									{
										/* Store object in variable */
										set_variable_object(global_scope, action->args[1], result->object_ptr, result->type, 0);
										/* Don't free result - the object pointer is now owned by the variable */
										free(result->name);
										free(result);
									}
								}
								else
								{
									unreal_log(ULOG_WARNING, "obbyscript", "DEBUG_BUILTIN_NULL", NULL,
										"Built-in function $func_name returned NULL",
										log_data_string("func_name", func_name));
								}
							}
							else
							{
								/* Regular user-defined function */
								char *return_value = NULL;
								if (execute_function(func_name, func_args, func_argc, client, channel, &return_value))
								{
									set_variable(global_scope, action->args[1], return_value, 0);
									if (return_value) free(return_value);
								}
							}
							
							/* Free function arguments */
							if (func_args)
							{
								for (int i = 0; i < func_argc; i++)
									free(func_args[i]);
								free(func_args);
							}
						}
					}
					free(func_call);
				}
				else
				{
					/* Regular variable assignment */
					char *value = substitute_variables(action->args[3], client, channel);
					set_variable(global_scope, action->args[1], value, 0);
					if (value) free(value);
				}
			}
			else if (action->argc >= 3 && strcmp(action->args[0], "var") == 0)
			{
				/* Format: var %name value (no equals sign) */
				unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_VAR_ASSIGN_SIMPLE", NULL,
					"Simple variable assignment: $var_name = $value",
					log_data_string("var_name", action->args[1]),
					log_data_string("value", action->args[2]));
				
				/* Check if value is an array literal */
				if (action->args[2] && action->args[2][0] == '[')
				{
					/* Parse and create array */
					ObbyScriptArray *array = parse_array_literal(action->args[2], client, channel);
					
					if (array)
					{
						set_variable_array(global_scope, action->args[1], array, 0);
					}
				}
				/* Check if value is $client.channels */
				else if (action->args[2] && strcmp(action->args[2], "$client.channels") == 0 && client)
				{
					ObbyScriptArray *array = get_client_channels(client);
					if (array)
					{
						set_variable_array(global_scope, action->args[1], array, 0);
					}
				}
				/* Check if value is a function call */
				else if (action->args[2] && is_function_call(action->args[2]))
				{
					unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_VAR_SIMPLE_FUNC_CALL", NULL,
						"Detected function call in simple assignment: $func_call",
						log_data_string("func_call", action->args[2]));
					
					/* Parse function call: $function_name(args...) or function_name(args...) */
					char *func_call = strdup(action->args[2]);
					char *func_name = func_call;
					
					/* Skip $ if present for user-defined functions */
					if (*func_name == '$')
						func_name++;
					
					char *args_start = strchr(func_name, '(');
					if (args_start)
					{
						*args_start = '\0'; /* Terminate function name */
						args_start++; /* Move to first argument */
						
						/* Find the closing parenthesis */
						char *args_end = strrchr(args_start, ')');
						if (args_end)
						{
							*args_end = '\0'; /* Terminate arguments */
							
							/* Parse arguments */
							char **func_args = NULL;
							int func_argc = 0;
							
							if (strlen(args_start) > 0)
							{
								/* Count commas to determine argument count */
								func_argc = 1;
								for (char *p = args_start; *p; p++)
								{
									if (*p == ',') func_argc++;
								}
								
								func_args = safe_alloc(sizeof(char*) * func_argc);
								char *arg_copy = strdup(args_start);
								char *saveptr;
								char *token = strtok_r(arg_copy, ",", &saveptr);
								int i = 0;
								
								while (token && i < func_argc)
								{
									/* Trim whitespace and quotes */
									while (*token && isspace(*token)) token++;
									if (*token == '"' && token[strlen(token)-1] == '"')
									{
										token++;
										token[strlen(token)-1] = '\0';
									}
									func_args[i] = substitute_variables(token, client, channel);
									if (!func_args[i])
										func_args[i] = strdup(token);
									i++;
									token = strtok_r(NULL, ",", &saveptr);
								}
								free(arg_copy);
							}
							
							/* Execute function and get return value */
							if (is_builtin_function(func_name))
							{
								unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_SIMPLE_BUILTIN_CALL", NULL,
									"Executing built-in function: $func_name with $argc args",
									log_data_string("func_name", func_name),
									log_data_integer("argc", func_argc));
								
								ObbyScriptVariable *result = execute_builtin_function(func_name, func_args, func_argc);
								if (result)
								{
									unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_SIMPLE_BUILTIN_RESULT", NULL,
										"Built-in function returned object of type $type, storing in variable $var_name",
										log_data_integer("type", result->type),
										log_data_string("var_name", action->args[1]));
									
									/* Check if result is a STRING (indicates not found - $false) */
									if (result->type == US_VAR_STRING && result->value)
									{
										/* Store the string value (e.g., "$false") */
										set_variable(global_scope, action->args[1], result->value, 0);
										free(result->name);
										if (result->value) free(result->value);
										free(result);
									}
									else
									{
										/* Store object in variable */
										set_variable_object(global_scope, action->args[1], result->object_ptr, result->type, 0);
										/* Don't free result - the object pointer is now owned by the variable */
										free(result->name);
										free(result);
									}
								}
								else
								{
									unreal_log(ULOG_WARNING, "obbyscript", "DEBUG_SIMPLE_BUILTIN_NULL", NULL,
										"Built-in function $func_name returned NULL",
										log_data_string("func_name", func_name));
								}
							}
							else
							{
								/* Regular user-defined function */
								char *return_value = NULL;
								if (execute_function(func_name, func_args, func_argc, client, channel, &return_value))
								{
									set_variable(global_scope, action->args[1], return_value, 0);
									if (return_value) free(return_value);
								}
							}
							
							/* Free function arguments */
							if (func_args)
							{
								for (int i = 0; i < func_argc; i++)
									free(func_args[i]);
								free(func_args);
							}
						}
					}
					free(func_call);
				}
				else
				{
					/* Regular variable assignment */
					char *value = substitute_variables(action->args[2], client, channel);
					set_variable(global_scope, action->args[1], value, 0);
					if (value) free(value);
				}
			}
			else if (action->argc == 1 && strstr(action->args[0], " = "))
			{
				/* Handle single-line variable assignment like "var %user = find_client($client.name)" */
				char *line = strdup(action->args[0]);
				char *var_part = line;
				char *equals = strstr(line, " = ");
				
				if (equals)
				{
					*equals = '\0'; /* Terminate variable part */
					char *value_part = equals + 3; /* Skip " = " */
					
					/* Extract variable name from "var %name" */
					char *var_token = strtok(var_part, " ");
					if (var_token && strcmp(var_token, "var") == 0)
					{
						char *var_name = strtok(NULL, " ");
						if (var_name)
						{
							unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_SINGLE_LINE_VAR", NULL,
								"Single-line variable assignment: $var_name = $value",
								log_data_string("var_name", var_name),
								log_data_string("value", value_part));
							
							/* Check if value is a function call */
							if (is_function_call(value_part))
							{
								/* Handle function call assignment */
								char *func_call = strdup(value_part);
								char *func_name = func_call;
								
								/* Skip $ if present for user-defined functions */
								if (*func_name == '$')
									func_name++;
								
								char *args_start = strchr(func_name, '(');
								if (args_start)
								{
									*args_start = '\0'; /* Terminate function name */
									args_start++; /* Move to first argument */
									
									/* Find the closing parenthesis */
									char *args_end = strrchr(args_start, ')');
									if (args_end)
									{
										*args_end = '\0'; /* Terminate arguments */
										
										/* Parse arguments */
										char **func_args = NULL;
										int func_argc = 0;
										
										if (strlen(args_start) > 0)
										{
											/* Count commas to determine argument count */
											func_argc = 1;
											for (char *p = args_start; *p; p++)
											{
												if (*p == ',') func_argc++;
											}
											
											func_args = safe_alloc(sizeof(char*) * func_argc);
											char *arg_copy = strdup(args_start);
											char *saveptr;
											char *token = strtok_r(arg_copy, ",", &saveptr);
											int i = 0;
											
											while (token && i < func_argc)
											{
												/* Trim whitespace and quotes */
												while (*token && isspace(*token)) token++;
												if (*token == '"' && token[strlen(token)-1] == '"')
												{
													token++;
													token[strlen(token)-1] = '\0';
												}
												func_args[i] = substitute_variables(token, client, channel);
												if (!func_args[i])
													func_args[i] = strdup(token);
												i++;
												token = strtok_r(NULL, ",", &saveptr);
											}
											free(arg_copy);
										}
										
										/* Execute function and get return value */
										if (is_builtin_function(func_name))
										{
											ObbyScriptVariable *result = execute_builtin_function(func_name, func_args, func_argc);
											if (result)
											{
												/* Check if result is a STRING (indicates not found - $false) */
												if (result->type == US_VAR_STRING && result->value)
												{
													/* Store the string value (e.g., "$false") */
													set_variable(global_scope, var_name, result->value, 0);
													free(result->name);
													if (result->value) free(result->value);
													free(result);
												}
												else
												{
													/* Store object in variable */
													set_variable_object(global_scope, var_name, result->object_ptr, result->type, 0);
													/* Don't free result - the object pointer is now owned by the variable */
													free(result->name);
													free(result);
												}
											}
										}
										else
										{
											/* Regular user-defined function */
											char *return_value = NULL;
											if (execute_function(func_name, func_args, func_argc, client, channel, &return_value))
											{
												set_variable(global_scope, var_name, return_value, 0);
												if (return_value) free(return_value);
											}
										}
										
										/* Free function arguments */
										if (func_args)
										{
											for (int i = 0; i < func_argc; i++)
												free(func_args[i]);
											free(func_args);
										}
									}
								}
								free(func_call);
							}
							else
							{
								/* Regular variable assignment */
								char *value = substitute_variables(value_part, client, channel);
								set_variable(global_scope, var_name, value, 0);
								if (value) free(value);
							}
						}
					}
				}
				free(line);
			}
			else if (action->argc >= 4 && strcmp(action->args[0], "const") == 0 && strcmp(action->args[1], "var") == 0)
			{
				/* const var %name value */
				char *value = substitute_variables(action->args[3], client, channel);
				set_variable(global_scope, action->args[2], value, 1);
				if (value) free(value);
			}
			else if (action->argc >= 3 && strchr(action->args[0], '[') != NULL && strchr(action->args[0], ']') != NULL)
			{
				/* %name[index] = value (array element assignment) */
				char *name_copy = strdup(action->args[0]);
				char *bracket_pos = strchr(name_copy, '[');
				if (bracket_pos)
				{
					*bracket_pos = '\0'; /* Terminate var name */
					char *index_start = bracket_pos + 1;
					char *index_end = strchr(index_start, ']');
					if (index_end)
					{
						*index_end = '\0'; /* Terminate index */
						
						/* Get the variable */
						const char *var_name = name_copy[0] == '%' ? name_copy + 1 : name_copy;
						ObbyScriptVariable *var = get_variable_object(global_scope, var_name);
						
						if (var && var->type == US_VAR_ARRAY && var->array_ptr)
						{
							/* Evaluate the index */
							char *index_value = substitute_variables(index_start, client, channel);
							if (index_value)
							{
								int index = atoi(index_value);
								free(index_value);
								
								/* Evaluate the value to assign */
								char *value = substitute_variables(action->args[2], client, channel);
								if (value)
								{
									array_set_string(var->array_ptr, index, value);
									free(value);
								}
							}
						}
					}
				}
				free(name_copy);
			}
			else if (action->argc >= 3 && strchr(action->args[0], '=') != NULL)
			{
				/* %name = value (assignment) */
				char *name = strtok(action->args[0], "=");
				char *value = substitute_variables(action->args[2], client, channel);
				set_variable(global_scope, name, value, 0);
				if (value) free(value);
			}
			break;
		}

		case US_ACTION_ARITHMETIC:
		{
			/* Handle arithmetic operations */
			if (action->argc >= 1)
			{
				char *line = action->args[0];
				char *var_name = NULL;
				char result_str[32];
				
				fprintf(stderr, "[EXEC_DEBUG] Executing arithmetic operation: '%s'\n", line);
				
				/* Extract variable name */
				if (line[0] == '%')
				{
					char *space_or_op = line + 1;
					while (*space_or_op && *space_or_op != '+' && *space_or_op != '-' && *space_or_op != '*' && *space_or_op != '/' && *space_or_op != '=' && !isspace(*space_or_op))
						space_or_op++;
					
					int var_len = space_or_op - (line + 1);
					var_name = safe_alloc(var_len + 1);
					strncpy(var_name, line + 1, var_len);
					var_name[var_len] = '\0';
				}
				
				if (var_name)
				{
					/* Get current value */
					char *current_val = get_variable(global_scope, var_name);
					int current_int = current_val ? atoi(current_val) : 0;
					int new_value = current_int;
					
					fprintf(stderr, "[EXEC_DEBUG] Variable %s: current=%d\n", var_name, current_int);
					
					/* Handle different arithmetic operations */
					if (strstr(line, "++"))
					{
						new_value = current_int + 1;
						fprintf(stderr, "[EXEC_DEBUG] Increment operation: %d -> %d\n", current_int, new_value);
					}
					else if (strstr(line, "--"))
					{
						new_value = current_int - 1;
					}
					else if (strstr(line, "+="))
					{
						char *expr_start = strstr(line, "+=") + 2;
						while (*expr_start && isspace(*expr_start)) expr_start++;
						int add_val = evaluate_arithmetic(expr_start, client, channel);
						new_value = current_int + add_val;
					}
					else if (strstr(line, "-="))
					{
						char *expr_start = strstr(line, "-=") + 2;
						while (*expr_start && isspace(*expr_start)) expr_start++;
						int sub_val = evaluate_arithmetic(expr_start, client, channel);
						new_value = current_int - sub_val;
					}
					else if (strstr(line, "*="))
					{
						char *expr_start = strstr(line, "*=") + 2;
						while (*expr_start && isspace(*expr_start)) expr_start++;
						int mul_val = evaluate_arithmetic(expr_start, client, channel);
						new_value = current_int * mul_val;
					}
					else if (strstr(line, "/="))
					{
						char *expr_start = strstr(line, "/=") + 2;
						while (*expr_start && isspace(*expr_start)) expr_start++;
						int div_val = evaluate_arithmetic(expr_start, client, channel);
						if (div_val != 0) new_value = current_int / div_val;
					}
					else if (strchr(line, '='))
					{
						/* Handle %var = expression */
						char *expr_start = strchr(line, '=') + 1;
						while (*expr_start && isspace(*expr_start)) expr_start++;
						new_value = evaluate_arithmetic(expr_start, client, channel);
					}
					
					/* Set the new value */
					snprintf(result_str, sizeof(result_str), "%d", new_value);
					set_variable(global_scope, var_name, result_str, 0);
					
					free(var_name);
				}
			}
			break;
		}

		case US_ACTION_ISUPPORT:
		{
			/* Add ISUPPORT token */
			if (action->argc >= 1 && action->args[0])
			{
				char *token_def = substitute_variables(action->args[0], client, channel);
				if (token_def)
				{
					/* Parse TOKEN=value format */
					char *equals = strchr(token_def, '=');
					if (equals)
					{
						*equals = '\0';
						char *token_name = token_def;
						char *token_value = equals + 1;
						
						/* Add to ISUPPORT */
						ISupportAdd(NULL, token_name, token_value);
					}
					else
					{
						/* Just token name without value */
						ISupportAdd(NULL, token_def, NULL);
					}
					free(token_def);
				}
			}
			break;
		}

		case US_ACTION_BREAK:
		{
			/* Break statement - set global flag for loop handling */
			should_break = 1;
			unreal_log(ULOG_INFO, "obbyscript", "BREAK_EXECUTED", NULL,
				"Break statement executed - setting global flag");
			break;
		}

		case US_ACTION_CONTINUE:
		{
			/* Continue statement - set global flag for loop handling */
			should_continue = 1;
			unreal_log(ULOG_INFO, "obbyscript", "CONTINUE_EXECUTED", NULL,
				"Continue statement executed - setting global flag");
			break;
		}

		case US_ACTION_CAP:
		{
			/* Add CAP capability to pending list for registration */
			if (action->argc >= 1 && action->args[0])
			{
				char *cap_name = substitute_variables(action->args[0], client, channel);
				if (cap_name)
				{
					/* Add to pending caps list */
					add_pending_cap(cap_name);
					free(cap_name);
				}
			}
			break;
		}

		case US_ACTION_FUNCTION_CALL:
		{
			/* Execute function call */
			if (action->function && action->argc >= 0)
			{
				/* Substitute variables in arguments, but handle object variables specially */
				char **substituted_args = NULL;
				ObbyScriptVariable **object_args = NULL;
				if (action->argc > 0)
				{
					substituted_args = safe_alloc(sizeof(char*) * action->argc);
					object_args = safe_alloc(sizeof(ObbyScriptVariable*) * action->argc);
					for (int i = 0; i < action->argc; i++)
					{
						object_args[i] = NULL;
						
						/* Check if argument is an object variable (starts with %) */
						if (action->args[i] && action->args[i][0] == '%')
						{
							/* Extract variable name (remove % prefix) */
							char *var_name = action->args[i] + 1;
							ObbyScriptVariable *var = get_variable_object(global_scope, var_name);
							if (var && (var->type == US_VAR_CLIENT || var->type == US_VAR_CHANNEL))
							{
								/* This is an object variable - pass it directly */
								object_args[i] = var;
								substituted_args[i] = strdup("__OBJECT__"); /* Placeholder */
								continue;
							}
						}
						
						/* Regular variable substitution */
						substituted_args[i] = substitute_variables(action->args[i], client, channel);
						if (!substituted_args[i])
							substituted_args[i] = strdup(action->args[i]);
					}
				}
				
				/* Execute the function with object-aware parameter passing */
				execute_function_with_objects(action->function, substituted_args, object_args, action->argc, client, channel, NULL);
				
				/* Free substituted arguments */
				if (substituted_args)
				{
					for (int i = 0; i < action->argc; i++)
						free(substituted_args[i]);
					free(substituted_args);
				}
				if (object_args)
				{
					free(object_args);
				}
			}
			break;
		}

		case US_ACTION_RETURN:
		{
			/* Handle return statement in functions */
			if (action->argc >= 1 && action->args[0])
			{
				char *return_value = substitute_variables(action->args[0], client, channel);
				unreal_log(ULOG_DEBUG, "obbyscript", "RETURN_VALUE_DEBUG", NULL,
					"Function return: original='$original' processed='$processed'",
					log_data_string("original", action->args[0] ? action->args[0] : "NULL"),
					log_data_string("processed", return_value ? return_value : "NULL"));
				if (return_value)
				{
					set_variable(global_scope, "__return__", return_value, 0);
					free(return_value);
				}
			}
			/* Don't execute next actions after return */
			return;
		}

		default:
			break;
	}

	/* Execute next action in sequence - EXCEPT for IF actions which handle their own flow */
	/* Also check if a return statement was executed */
	if (action->next && action->type != US_ACTION_IF)
	{
		char *return_val = get_variable(global_scope, "__return__");
		if (!return_val)
		{
			/* No return executed, continue to next action */
			execute_script_action(action->next, client, channel);
		}
		/* If return_val exists, stop execution */
	}
}

void execute_script_action_with_params(ObbyScriptAction *action, Client *client, Channel *channel, int parc, const char *parv[])
{
	/* Set global command context for parameter substitution */
	int saved_parc = current_command_parc;
	const char **saved_parv = current_command_parv;
	
	current_command_parc = parc;
	current_command_parv = parv;
	
	/* Execute the action (parameter substitution will happen in substitute_variables) */
	execute_script_action(action, client, channel);
	
	/* Restore previous context */
	current_command_parc = saved_parc;
	current_command_parv = saved_parv;
}

int evaluate_condition(ObbyScriptCondition *condition, Client *client, Channel *channel)
{
	/* Simple condition evaluation */
	if (!condition)
		return 0;
	
	/* Debug: Log what condition we're evaluating */
	unreal_log(ULOG_DEBUG, "obbyscript", "EVAL_CONDITION_START", client,
		"evaluate_condition called: operator='$op', variable='$var', value='$val'",
		log_data_string("op", condition->operator ? condition->operator : "NULL"),
		log_data_string("var", condition->variable ? condition->variable : "NULL"),
		log_data_string("val", condition->value ? condition->value : "NULL"));
	
	/* Special case: simple variable evaluation like if ($client.user.account) */
	if (!condition->operator || strlen(condition->operator) == 0)
	{
		char *value = evaluate_condition_value(condition->variable, client, channel);
		if (value)
		{
			int result = !is_falsy_value(value);
			free(value);
			return result;
		}
		return 0;
	}

	if (!client)
		return 0;

	if (strcmp(condition->variable, "$client.umodes") == 0)
	{
		if (strcmp(condition->operator, "has") == 0)
		{
			if (strcmp(condition->value, "UMODE_OPER") == 0)
				return IsOper(client);
		}
		else if (strcmp(condition->operator, "!has") == 0)
		{
			if (strcmp(condition->value, "UMODE_OPER") == 0)
				return !IsOper(client);
		}
	}
	else if (strcmp(condition->variable, "$client.name") == 0)
	{
		if (strcmp(condition->operator, "==") == 0)
		{
			/* Check if client name equals the specified value */
			int result = (condition->value && client->name && strcmp(client->name, condition->value) == 0) ? 1 : 0;
			return result;
		}
		else if (strcmp(condition->operator, "!=") == 0)
		{
			/* Check if client name does NOT equal the specified value */
			int result = (condition->value && client->name && strcmp(client->name, condition->value) != 0) ? 1 : 0;
			return result;
		}
	}
	else if (strcmp(condition->variable, "$client") == 0)
	{
		if (strcmp(condition->operator, "hascap") == 0)
		{
			/* Check if client has the specified capability */
			if (condition->value && HasCapability(client, condition->value))
				return 1;
			return 0;
		}
		else if (strcmp(condition->operator, "!hascap") == 0)
		{
			/* Check if client does NOT have the specified capability */
			if (condition->value && !HasCapability(client, condition->value))
				return 1;
			return 0;
		}
		else if (strcmp(condition->operator, "ischanop") == 0)
		{
			/* Check if client is a channel operator (+o) */
			if (!client || !channel)
			{
				return 0;
			}
			
			int result = check_channel_access(client, channel, "o") ? 1 : 0;
			return result;
		}
		else if (strcmp(condition->operator, "isvoice") == 0)
		{
			/* Check if client has voice (+v) */
			int result = (channel && check_channel_access(client, channel, "v")) ? 1 : 0;
			return result;
		}
		else if (strcmp(condition->operator, "ishalfop") == 0)
		{
			/* Check if client is a halfop (+h) */
			int result = (channel && check_channel_access(client, channel, "h")) ? 1 : 0;
			return result;
		}
		else if (strcmp(condition->operator, "isadmin") == 0)
		{
			/* Check if client is a channel admin (+a) */
			int result = (channel && check_channel_access(client, channel, "a")) ? 1 : 0;
			return result;
		}
		else if (strcmp(condition->operator, "isowner") == 0)
		{
			/* Check if client is a channel owner (+q) */
			int result = (channel && check_channel_access(client, channel, "q")) ? 1 : 0;
			return result;
		}
		else if (strcmp(condition->operator, "in") == 0)
		{
			/* Check if client is in (on) the specified channel */
			if (!condition->value)
				return 0;
			
			/* Resolve channel - could be $chan variable, %channel_var, or a channel name */
			Channel *target_channel = NULL;
			
			if (strcmp(condition->value, "$chan") == 0 || strcmp(condition->value, "$channel") == 0)
			{
				/* Use the channel from context */
				target_channel = channel;
			}
			else if (condition->value[0] == '%')
			{
				/* This is a script variable - look up the object */
				char *var_name = condition->value + 1; /* Skip % */
				ObbyScriptVariable *var = get_variable_object(global_scope, var_name);
				if (var && var->type == US_VAR_CHANNEL && var->object_ptr)
				{
					target_channel = (Channel *)var->object_ptr;
				}
				/* If var is a STRING (e.g., "$false"), target_channel stays NULL */
			}
			else
			{
				/* Evaluate the channel value (could be a variable or literal) */
				char *chan_value = evaluate_condition_value(condition->value, client, channel);
				if (chan_value)
				{
					/* If it starts with #, treat as channel name */
					if (chan_value[0] == '#')
						target_channel = find_channel(chan_value);
					free(chan_value);
				}
			}
			
			if (!target_channel)
				return 0;
			
			/* Use IsMember macro to check if client is on channel */
			return IsMember(client, target_channel) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "insg") == 0)
		{
			/* Check if client is in the specified security group */
			unreal_log(ULOG_DEBUG, "obbyscript", "INSG_CONDITION_DEBUG", client,
				"insg operator: condition->value='$value'",
				log_data_string("value", condition->value ? condition->value : "NULL"));
			
			if (!condition->value)
				return 0;
			
			/* Evaluate the security group name (could be a variable or literal) */
			char *sg_value = evaluate_condition_value(condition->value, client, channel);
			
			unreal_log(ULOG_DEBUG, "obbyscript", "INSG_EVAL_DEBUG", client,
				"insg operator: sg_value after evaluation='$sg_value'",
				log_data_string("sg_value", sg_value ? sg_value : "NULL"));
			
			if (!sg_value)
				return 0;
			
			unreal_log(ULOG_DEBUG, "obbyscript", "INSG_CHECK_DEBUG", client,
				"Checking if client $client.name is in security group '$sg_name'",
				log_data_string("sg_name", sg_value));
			
			/* Use user_allowed_by_security_group_name to check membership */
			int raw_result = user_allowed_by_security_group_name(client, sg_value);
			int result = raw_result ? 1 : 0;
			
			unreal_log(ULOG_DEBUG, "obbyscript", "INSG_RESULT_DEBUG", client,
				"Security group check result: raw=$raw, final=$final",
				log_data_integer("raw", raw_result),
				log_data_integer("final", result));
			
			free(sg_value);
			return result;
		}
		/* New IRC-related operators */
		else if (strcmp(condition->operator, "isoper") == 0)
		{
			return IsOper(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isinvisible") == 0)
		{
			return IsInvisible(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isregnick") == 0)
		{
			return IsRegNick(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "ishidden") == 0)
		{
			return IsHidden(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "ishideoper") == 0)
		{
			return IsHideOper(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "issecure") == 0)
		{
			return IsSecure(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isuline") == 0)
		{
			return IsULine(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isloggedin") == 0)
		{
			return IsLoggedIn(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isserver") == 0)
		{
			return IsServer(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isquarantined") == 0)
		{
			return IsQuarantined(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isshunned") == 0)
		{
			return IsShunned(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isvirus") == 0)
		{
			return IsVirus(client) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isinvited") == 0)
		{
			if (!channel)
				return 0;
			return is_invited(client, channel) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "isbanned") == 0)
		{
			if (!channel)
				return 0;
			/* Check if user is banned from channel */
			return is_banned(client, channel, BANCHK_JOIN, NULL, NULL) ? 1 : 0;
		}
		else if (strcmp(condition->operator, "hasaccess") == 0)
		{
			if (!channel || !condition->value)
				return 0;
			return check_channel_access(client, channel, condition->value) ? 1 : 0;
		}
	}
	else if (strcmp(condition->operator, "in") == 0 && strcmp(condition->variable, "$client") != 0)
	{
		/* Check if client is in (on) the specified channel - fallback for non-$client variables */
		if (!condition->value)
			return 0;
		
		/* Resolve channel - could be $chan variable or a channel name */
		Channel *target_channel = NULL;
		
		if (strcmp(condition->value, "$chan") == 0 || strcmp(condition->value, "$channel") == 0)
		{
			/* Use the channel from context */
			target_channel = channel;
		}
		else
		{
			/* Evaluate the channel value (could be a variable or literal) */
			char *chan_value = evaluate_condition_value(condition->value, client, channel);
			if (chan_value)
			{
				/* If it starts with #, treat as channel name */
				if (chan_value[0] == '#')
					target_channel = find_channel(chan_value);
				free(chan_value);
			}
		}
		
		if (!target_channel)
			return 0;
		
		/* Use IsMember macro to check if client is on channel */
		return IsMember(client, target_channel) ? 1 : 0;
	}
	else
	{
		/* Generic variable comparison */
		if (strcmp(condition->operator, "==") == 0)
		{
			char *var_value = evaluate_condition_value(condition->variable, client, channel);
			char *cmp_value = evaluate_condition_value(condition->value, client, channel);
			
			/* Convert boolean and null literals to normalized values for comparison */
			char *normalized_var = var_value;
			char *normalized_cmp = cmp_value;
			
			if (var_value && (strcmp(var_value, "$true") == 0 || strcmp(var_value, "$false") == 0))
			{
				normalized_var = strdup(strcmp(var_value, "$true") == 0 ? "true" : "false");
			}
			else if (var_value && strcmp(var_value, "$null") == 0)
			{
				normalized_var = strdup("__NULL__");
			}
			
			if (cmp_value && (strcmp(cmp_value, "$true") == 0 || strcmp(cmp_value, "$false") == 0))
			{
				normalized_cmp = strdup(strcmp(cmp_value, "$true") == 0 ? "true" : "false");
			}
			else if (cmp_value && strcmp(cmp_value, "$null") == 0)
			{
				normalized_cmp = strdup("__NULL__");
			}
			
			/* Debug: Log the comparison values */
			unreal_log(ULOG_INFO, "obbyscript", "CONDITION_EVAL_DEBUG", client,
				"Evaluating equality condition: '$var_value' == '$cmp_value' (normalized: '$normalized_var' == '$normalized_cmp')",
				log_data_string("var_value", var_value ? var_value : "NULL"),
				log_data_string("cmp_value", cmp_value ? cmp_value : "NULL"),
				log_data_string("normalized_var", normalized_var ? normalized_var : "NULL"),
				log_data_string("normalized_cmp", normalized_cmp ? normalized_cmp : "NULL"));
			
			int result = 0;
			if (normalized_var && normalized_cmp)
				result = (strcmp(normalized_var, normalized_cmp) == 0) ? 1 : 0;
			else if (!normalized_var && !normalized_cmp)
				result = 1;
			
			unreal_log(ULOG_INFO, "obbyscript", "CONDITION_RESULT", client,
				"Comparison result: $result",
				log_data_integer("result", result));
			
			/* Free normalized values if they were allocated */
			if (normalized_var != var_value && normalized_var) free(normalized_var);
			if (normalized_cmp != cmp_value && normalized_cmp) free(normalized_cmp);
			
			if (var_value) free(var_value);
			if (cmp_value) free(cmp_value);
			return result;
		}
		else if (strcmp(condition->operator, "!=") == 0)
		{
			char *var_value = evaluate_condition_value(condition->variable, client, channel);
			char *cmp_value = evaluate_condition_value(condition->value, client, channel);
			
			/* Convert boolean and null literals to normalized values for comparison */
			char *normalized_var = var_value;
			char *normalized_cmp = cmp_value;
			
			if (var_value && (strcmp(var_value, "$true") == 0 || strcmp(var_value, "$false") == 0))
			{
				normalized_var = strdup(strcmp(var_value, "$true") == 0 ? "true" : "false");
			}
			else if (var_value && strcmp(var_value, "$null") == 0)
			{
				normalized_var = strdup("__NULL__");
			}
			
			if (cmp_value && (strcmp(cmp_value, "$true") == 0 || strcmp(cmp_value, "$false") == 0))
			{
				normalized_cmp = strdup(strcmp(cmp_value, "$true") == 0 ? "true" : "false");
			}
			else if (cmp_value && strcmp(cmp_value, "$null") == 0)
			{
				normalized_cmp = strdup("__NULL__");
			}
			
			/* Debug: Log the comparison values */
			unreal_log(ULOG_DEBUG, "obbyscript", "CONDITION_COMPARISON_DEBUG", client,
				"Comparing values for != : '$var_value' != '$cmp_value' (normalized: '$normalized_var' != '$normalized_cmp')",
				log_data_string("var_value", var_value ? var_value : "NULL"),
				log_data_string("cmp_value", cmp_value ? cmp_value : "NULL"),
				log_data_string("normalized_var", normalized_var ? normalized_var : "NULL"),
				log_data_string("normalized_cmp", normalized_cmp ? normalized_cmp : "NULL"));
			
			int result = 0;
			if (normalized_var && normalized_cmp)
			{
				result = (strcmp(normalized_var, normalized_cmp) != 0) ? 1 : 0;
				unreal_log(ULOG_DEBUG, "obbyscript", "CONDITION_RESULT_DEBUG", client,
					"String comparison result: strcmp('$normalized_var', '$normalized_cmp') = $strcmp_result, final result = $result",
					log_data_string("normalized_var", normalized_var),
					log_data_string("normalized_cmp", normalized_cmp),
					log_data_integer("strcmp_result", strcmp(normalized_var, normalized_cmp)),
					log_data_integer("result", result));
			}
			else if (normalized_var || normalized_cmp)
				result = 1;
			
			/* Free normalized values if they were allocated */
			if (normalized_var != var_value && normalized_var) free(normalized_var);
			if (normalized_cmp != cmp_value && normalized_cmp) free(normalized_cmp);
			
			if (var_value) free(var_value);
			if (cmp_value) free(cmp_value);
			return result;
		}
	}

	return 0;
}

int obbyscript_can_join(Client *client, Channel *channel, const char *key, char **errmsg)
{
	ObbyScriptFile *file;
	ObbyScriptRule *rule;
	ObbyScriptChannelSnapshot *channel_snapshot = NULL;
	
	if (!script_files || !client || !channel)
		return 0;
		
	/* Create channel snapshot for safe access */
	channel_snapshot = safe_alloc(sizeof(ObbyScriptChannelSnapshot));
	if (channel->name)
		channel_snapshot->name = strdup(channel->name);
	if (channel->topic)
		channel_snapshot->topic = strdup(channel->topic);
	channel_snapshot->user_count = channel->users ? channel->users : 0;
		
	/* Check CAN_JOIN event scripts */
	for (file = script_files; file; file = file->next)
	{
		if (!file->rules)
			continue;
			
		for (rule = file->rules; rule; rule = rule->next)
		{
			if (rule->event == US_EVENT_CAN_JOIN)
			{
				/* Check if target matches */
				if (!rule->target || strlen(rule->target) == 0)
					continue;
					
				int target_matches = 0;
				
				if (strcmp(rule->target, "*") == 0)
				{
					target_matches = 1;
				}
				else if (channel->name && strcmp(rule->target, channel->name) == 0)
				{
					target_matches = 1;
				}
				
				if (target_matches && rule->actions)
				{
					/* Clear any previous return value */
					set_variable(global_scope, "__return", "", 0);
					
					/* Execute the actions and check for return values */
					ObbyScriptAction *action = rule->actions;
					while (action)
					{
						execute_script_action(action, client, channel);
						
						/* Check if a return value was set by an if statement after EACH action */
						char *return_val = get_variable(global_scope, "__return");
						if (return_val && strlen(return_val) > 0)
						{
							int should_block = (strcmp(return_val, "false") == 0);
							int should_allow = (strcmp(return_val, "true") == 0);
							
							if (should_block || should_allow)
							{
								/* Clean up */
								if (channel_snapshot)
								{
									if (channel_snapshot->name) free(channel_snapshot->name);
									if (channel_snapshot->topic) free(channel_snapshot->topic);
									free(channel_snapshot);
								}
								
								if (should_block)
								{
									*errmsg = STR_ERR_BANNEDFROMCHAN;
									return ERR_BANNEDFROMCHAN;
								}
								else
								{
									return 0;
								}
							}
						}
						action = action->next;
						
						/* Check if we've reached the end of the action list */
						if (!action)
							break;
						
						/* Handle return statements specially */
						if (action->type == US_ACTION_RETURN && action->argc > 0)
						{
							if (strcmp(action->args[0], "$false") == 0)
							{
								/* Clean up and block */
								if (channel_snapshot)
								{
									if (channel_snapshot->name) free(channel_snapshot->name);
									if (channel_snapshot->topic) free(channel_snapshot->topic);
									free(channel_snapshot);
								}
								*errmsg = STR_ERR_BANNEDFROMCHAN;
								return ERR_BANNEDFROMCHAN;
							}
							else if (strcmp(action->args[0], "$true") == 0)
							{
								/* Clean up and allow */
								if (channel_snapshot)
								{
									if (channel_snapshot->name) free(channel_snapshot->name);
									if (channel_snapshot->topic) free(channel_snapshot->topic);
									free(channel_snapshot);
								}
								return 0;
							}
						}
						/* Handle IF statements with nested returns */
						else if (action->type == US_ACTION_IF && action->condition)
						{
							unreal_log(ULOG_DEBUG, "obbyscript", "CONDITION_EXECUTE_DEBUG", NULL,
								"About to evaluate condition - variable: '$variable', operator: '$operator', value: '$value'",
								log_data_string("variable", action->condition->variable ? action->condition->variable : "NULL"),
								log_data_string("operator", action->condition->operator ? action->condition->operator : "NULL"),
								log_data_string("value", action->condition->value ? action->condition->value : "NULL"));
							
							if (evaluate_condition(action->condition, client, channel))
							{
								ObbyScriptAction *nested = action->nested_actions;
								while (nested)
								{
									if (nested->type == US_ACTION_RETURN && nested->argc > 0)
									{
										if (strcmp(nested->args[0], "$false") == 0)
										{
											/* Clean up and block */
											if (channel_snapshot)
											{
												if (channel_snapshot->name) free(channel_snapshot->name);
												if (channel_snapshot->topic) free(channel_snapshot->topic);
												free(channel_snapshot);
											}
											*errmsg = STR_ERR_BANNEDFROMCHAN;
											return ERR_BANNEDFROMCHAN;
										}
										else if (strcmp(nested->args[0], "$true") == 0)
										{
											/* Clean up and allow */
											if (channel_snapshot)
											{
												if (channel_snapshot->name) free(channel_snapshot->name);
												if (channel_snapshot->topic) free(channel_snapshot->topic);
												free(channel_snapshot);
											}
											return 0;
										}
									}
									else
									{
										/* Execute non-return actions normally */
										execute_script_action(nested, client, NULL);
									}
									nested = nested->next;
								}
							}
						}
						else
						{
							/* Execute non-return actions normally */
							execute_script_action(action, client, NULL);
						}
						action = action->next;
					}
				}
			}
		}
	}
	
	/* Clean up */
	if (channel_snapshot)
	{
		if (channel_snapshot->name) free(channel_snapshot->name);
		if (channel_snapshot->topic) free(channel_snapshot->topic);
		free(channel_snapshot);
	}
	
	return 0;  /* Allow by default */
}

int obbyscript_local_join(Client *client, Channel *channel, MessageTag *mtags)
{
	in_join_context = 1;
	execute_scripts_for_event(US_EVENT_JOIN, client, channel, NULL);
	in_join_context = 0;
	
	/* Don't execute deferred actions immediately - they'll be executed 
	 * by a timer or other mechanism to avoid timing issues with other modules */
	
	return 0;
}

int obbyscript_remote_join(Client *client, Channel *channel, MessageTag *mtags)
{
	execute_scripts_for_event(US_EVENT_JOIN, client, channel, NULL);
	return 0;
}

int obbyscript_local_part(Client *client, Channel *channel, MessageTag *mtags, const char *comment)
{
	execute_scripts_for_event(US_EVENT_PART, client, channel, comment);
	return 0;
}

int obbyscript_remote_part(Client *client, Channel *channel, MessageTag *mtags, const char *comment)
{
	execute_scripts_for_event(US_EVENT_PART, client, channel, comment);
	return 0;
}

int obbyscript_local_quit(Client *client, MessageTag *mtags, const char *comment)
{
	execute_scripts_for_event(US_EVENT_QUIT, client, NULL, comment);
	return 0;
}

int obbyscript_remote_quit(Client *client, MessageTag *mtags, const char *comment)
{
	execute_scripts_for_event(US_EVENT_QUIT, client, NULL, comment);
	return 0;
}

int obbyscript_local_kick(Client *client, Client *victim, Channel *channel, MessageTag *mtags, const char *comment)
{
	execute_scripts_for_event(US_EVENT_KICK, victim, channel, comment);
	return 0;
}

int obbyscript_remote_kick(Client *client, Client *victim, Channel *channel, MessageTag *mtags, const char *comment)
{
	execute_scripts_for_event(US_EVENT_KICK, victim, channel, comment);
	return 0;
}

/* Connection hooks */
int obbyscript_local_connect(Client *client)
{
	execute_scripts_for_event(US_EVENT_CONNECT, client, NULL, NULL);
	return 0;
}

int obbyscript_remote_connect(Client *client)
{
	execute_scripts_for_event(US_EVENT_CONNECT, client, NULL, NULL);
	return 0;
}

/* Nick change hooks */
int obbyscript_local_nickchange(Client *client, MessageTag *mtags, const char *oldnick)
{
	execute_scripts_for_event(US_EVENT_NICK, client, NULL, oldnick);
	return 0;
}

int obbyscript_remote_nickchange(Client *client, MessageTag *mtags, const char *oldnick)
{
	execute_scripts_for_event(US_EVENT_NICK, client, NULL, oldnick);
	return 0;
}

/* Message hooks */
int obbyscript_chanmsg(Client *client, Channel *channel, int sendflags, const char *member_modes, const char *target, MessageTag *mtags, const char *text, SendType sendtype)
{
	execute_scripts_for_event(US_EVENT_PRIVMSG, client, channel, text);
	return 0;
}

int obbyscript_usermsg(Client *client, Client *to, MessageTag *mtags, const char *text, SendType sendtype)
{
	execute_scripts_for_event(US_EVENT_PRIVMSG, client, NULL, text);
	return 0;
}

/* Topic hook */
int obbyscript_topic(Client *client, Channel *channel, MessageTag *mtags, const char *topic)
{
	execute_scripts_for_event(US_EVENT_TOPIC, client, channel, topic);
	return 0;
}

/* Mode hooks */
int obbyscript_local_chanmode(Client *client, Channel *channel, MessageTag *mtags, const char *modebuf, const char *parabuf, time_t sendts, int samode, int *destroy_channel)
{
	execute_scripts_for_event(US_EVENT_CHANMODE, client, channel, modebuf);
	return 0;
}

int obbyscript_remote_chanmode(Client *client, Channel *channel, MessageTag *mtags, const char *modebuf, const char *parabuf, time_t sendts, int samode, int *destroy_channel)
{
	execute_scripts_for_event(US_EVENT_CHANMODE, client, channel, modebuf);
	return 0;
}

/* Invite and Knock hooks */
int obbyscript_invite(Client *client, Client *target, Channel *channel, MessageTag *mtags)
{
	execute_scripts_for_event(US_EVENT_INVITE, client, channel, target->name);
	return 0;
}

int obbyscript_knock(Client *client, Channel *channel, MessageTag *mtags, const char *comment)
{
	execute_scripts_for_event(US_EVENT_KNOCK, client, channel, comment);
	return 0;
}

/* Away hook */
int obbyscript_away(Client *client, MessageTag *mtags, const char *reason, int returning)
{
	execute_scripts_for_event(US_EVENT_AWAY, client, NULL, reason);
	return 0;
}

/* Oper hook */
int obbyscript_local_oper(Client *client, int add, const char *oper_block, const char *operclass)
{
	if (add)
		execute_scripts_for_event(US_EVENT_OPER, client, NULL, oper_block);
	return 0;
}

/* Kill hook */
int obbyscript_local_kill(Client *client, Client *victim, const char *reason)
{
	execute_scripts_for_event(US_EVENT_KILL, victim, NULL, reason);
	return 0;
}

/* User mode change hook */
int obbyscript_umode_change(Client *client, long setflags, long newflags)
{
	execute_scripts_for_event(US_EVENT_UMODE_CHANGE, client, NULL, NULL);
	return 0;
}

/* Channel create/destroy hooks */
int obbyscript_channel_create(Channel *channel)
{
	execute_scripts_for_event(US_EVENT_CHANNEL_CREATE, NULL, channel, NULL);
	return 0;
}

int obbyscript_channel_destroy(Channel *channel, int *should_destroy)
{
	execute_scripts_for_event(US_EVENT_CHANNEL_DESTROY, NULL, channel, NULL);
	return 0;
}

/* WHOIS hook */
int obbyscript_whois(Client *client, Client *target, NameValuePrioList **list)
{
	execute_scripts_for_event(US_EVENT_WHOIS, target, NULL, client->name);
	return 0;
}

/* Rehash hook */
int obbyscript_rehash(void)
{
	execute_scripts_for_event(US_EVENT_REHASH, NULL, NULL, NULL);
	return 0;
}

/* Account login hook */
int obbyscript_account_login(Client *client, MessageTag *mtags)
{
	execute_scripts_for_event(US_EVENT_ACCOUNT_LOGIN, client, NULL, NULL);
	return 0;
}

/* Command hooks */
int obbyscript_pre_command(Client *from, MessageTag *mtags, const char *buf)
{
	execute_scripts_for_event(US_EVENT_PRE_COMMAND, from, NULL, buf);
	return 0;
}

int obbyscript_post_command(Client *from, MessageTag *mtags, const char *buf)
{
	execute_scripts_for_event(US_EVENT_POST_COMMAND, from, NULL, buf);
	
	/* Special handling for JOIN commands - execute deferred actions after JOIN processing is complete */
	if (buf && from && MyConnect(from))
	{
		char *cmd_copy = strdup(buf);
		if (cmd_copy)
		{
			char *command = strtok(cmd_copy, " ");
			if (command && strcasecmp(command, "JOIN") == 0)
			{
				/* JOIN command completed - now it's safe to execute deferred actions */
				execute_deferred_actions();
			}
			free(cmd_copy);
		}
	}
	
	return 0;
}

/* TKL hooks */
int obbyscript_tkl_add(Client *client, TKL *tkl)
{
	execute_scripts_for_event(US_EVENT_TKL_ADD, client, NULL, NULL);
	return 0;
}

int obbyscript_tkl_del(Client *client, TKL *tkl)
{
	execute_scripts_for_event(US_EVENT_TKL_DEL, client, NULL, NULL);
	return 0;
}

/* Helper function to execute scripts for a given event */
void execute_scripts_for_event(ObbyScriptEventType event, Client *client, Channel *channel, const char *extra_data)
{
	ObbyScriptFile *file;
	ObbyScriptRule *rule;
	char *saved_channel_name = NULL;
	ObbyScriptChannelSnapshot *channel_snapshot = NULL;

	/* Basic safety checks */
	if (!script_files)
		return;
	
	/* Create a snapshot of channel data to avoid use-after-free issues */
	if (channel)
	{
		channel_snapshot = safe_alloc(sizeof(ObbyScriptChannelSnapshot));
		if (channel->name)
			channel_snapshot->name = strdup(channel->name);
		if (channel->topic)
			channel_snapshot->topic = strdup(channel->topic);
		channel_snapshot->user_count = channel->users ? channel->users : 0;
		
		/* Also save channel name for target matching */
		saved_channel_name = strdup(channel->name);
	}

	for (file = script_files; file; file = file->next)
	{
		if (!file->rules)
			continue;
			
		for (rule = file->rules; rule; rule = rule->next)
		{
			if (rule->event == event)
			{
				/* Check if target matches */
				if (!rule->target)
					continue;
				
				/* Additional safety check for rule target */
				if (strlen(rule->target) == 0)
					continue;
					
				/* More defensive checks for strcmp */
				int target_matches = 0;
				
				if (strcmp(rule->target, "*") == 0)
				{
					target_matches = 1;
				}
				else if (saved_channel_name && saved_channel_name[0] != '\0')
				{
					if (strcmp(rule->target, saved_channel_name) == 0)
						target_matches = 1;
				}
				else if (client && client->name && client->name[0] != '\0')
				{
					if (strcmp(rule->target, client->name) == 0)
						target_matches = 1;
				}
				
				if (target_matches)
				{
					if (rule->actions)
					{
						/* Execute with original channel pointer - substitute_variables will handle safety */
						execute_script_action(rule->actions, client, channel);
					}
				}
			}
		}
	}
	
	/* Clean up saved channel name */
	if (saved_channel_name)
		free(saved_channel_name);
		
	/* Clean up channel snapshot */
	if (channel_snapshot)
	{
		if (channel_snapshot->name)
			free(channel_snapshot->name);
		if (channel_snapshot->topic)
			free(channel_snapshot->topic);
		free(channel_snapshot);
	}
	
	/* NOTE: Deferred actions are no longer executed immediately to avoid use-after-free issues.
	 * Instead, rely on CAN_JOIN hooks to prevent problematic joins, or implement timer-based execution. */
	/* execute_deferred_actions(); */
}

/* Check if a command is destructive and should be deferred */
int is_destructive_command(const char *command)
{
	if (!command)
		return 0;
		
	/* Commands that can free channels/clients should be deferred */
	if (strcasecmp(command, "KICK") == 0 ||
	    strcasecmp(command, "KILL") == 0 ||
	    strcasecmp(command, "KLINE") == 0 ||
	    strcasecmp(command, "GLINE") == 0 ||
	    strcasecmp(command, "ZLINE") == 0 ||
	    strcasecmp(command, "SHUN") == 0)
	{
		return 1;
	}
	
	/* Commands that can trigger recursive JOINs should be deferred when in JOIN context */
	if (in_join_context)
	{
		if (strcasecmp(command, "SVSJOIN") == 0 ||
		    strcasecmp(command, "SAJOIN") == 0 ||
		    strcasecmp(command, "JOIN") == 0)
		{
			return 1;
		}
	}
	
	return 0;
}

/* Add a deferred action to be executed later */
void add_deferred_action(const char *command, const char **args, int argc, Client *client, Channel *channel)
{
	if (!command)
		return;
		
	ObbyScriptDeferredAction *action = safe_alloc(sizeof(ObbyScriptDeferredAction));
	action->command = strdup(command);
	
	/* Store client and channel names for lookup later */
	action->client_name = client ? strdup(client->name) : NULL;
	action->channel_name = channel ? strdup(channel->name) : NULL;
	
	if (argc > 0 && args)
	{
		action->args = safe_alloc(sizeof(char*) * argc);
		action->argc = argc;
		for (int i = 0; i < argc; i++)
		{
			if (args[i])
				action->args[i] = strdup(args[i]);
		}
	}
	
	/* Add to head of list */
	action->next = deferred_actions;
	deferred_actions = action;
}

/* Execute all deferred actions */
void execute_deferred_actions()
{
	ObbyScriptDeferredAction *action, *next;
	
	/* Prevent recursive execution */
	if (executing_deferred_actions)
		return;
		
	executing_deferred_actions = 1;
	
	for (action = deferred_actions; action; action = next)
	{
		next = action->next;
		
		if (action->command)
		{
			/* Look up client and channel by name */
			Client *client = action->client_name ? find_client(action->client_name, NULL) : NULL;
			Channel *channel = action->channel_name ? find_channel(action->channel_name) : NULL;
			
			/* Skip if client/channel no longer exists (they may have quit/been destroyed) */
			if ((action->client_name && !client) || (action->channel_name && !channel))
			{
				unreal_log(ULOG_DEBUG, "obbyscript", "DEFERRED_SKIP", NULL,
					"Skipping deferred action '$command' - client or channel no longer exists",
					log_data_string("command", action->command));
				continue;
			}
			
			/* Execute via do_cmd with proper server context */
			const char *parv[25];
			int parc = 0;
			
			/* Set up parv array */
			parv[parc++] = NULL;  /* parv[0] should be NULL */
			
			for (int i = 0; i < action->argc && parc < 24; i++)
			{
				if (action->args[i])
				{
					/* Do variable substitution on each argument using the looked-up client/channel */
					char *substituted = substitute_variables(action->args[i], client, channel);
					parv[parc++] = substituted ? substituted : action->args[i];
				}
			}
			parv[parc] = NULL;
			
			/* Execute the deferred command */
			do_cmd(&me, NULL, action->command, parc, parv);
			
			/* Note: We don't free the substituted strings because do_cmd may reference them
			 * This creates a small memory leak but prevents crashes */
		}
	}
	
	/* Clean up */
	free_deferred_actions();
	
	/* Reset the flag */
	executing_deferred_actions = 0;
}

/* Free all deferred actions */
void free_deferred_actions(void)
{
	ObbyScriptDeferredAction *action, *next;
	
	for (action = deferred_actions; action; action = next)
	{
		next = action->next;
		
		if (action->command)
			free(action->command);
			
		if (action->client_name)
			free(action->client_name);
			
		if (action->channel_name)
			free(action->channel_name);
			
		if (action->args)
		{
			for (int i = 0; i < action->argc; i++)
			{
				if (action->args[i])
					free(action->args[i]);
			}
			free(action->args);
		}
		
		free(action);
	}
	
	deferred_actions = NULL;
}

/* Variable management functions */
ObbyScriptScope *create_scope(ObbyScriptScope *parent)
{
	ObbyScriptScope *scope = safe_alloc(sizeof(ObbyScriptScope));
	scope->variables = NULL;
	scope->parent = parent;
	return scope;
}

void free_scope(ObbyScriptScope *scope)
{
	if (!scope)
		return;
		
	ObbyScriptVariable *var = scope->variables;
	while (var)
	{
		ObbyScriptVariable *next = var->next;
		if (var->name) free(var->name);
		if (var->value) free(var->value);
		if (var->array_ptr) free_array(var->array_ptr);
		free(var);
		var = next;
	}
	
	free(scope);
}

void set_variable(ObbyScriptScope *scope, const char *name, const char *value, int is_const)
{
	if (!scope || !name)
		return;
		
	/* Remove % prefix if present */
	const char *clean_name = (name[0] == '%') ? name + 1 : name;
	
	/* Check if variable already exists */
	ObbyScriptVariable *existing = find_variable(scope, clean_name);
	if (existing)
	{
		if (existing->is_const)
		{
			unreal_log(ULOG_WARNING, "obbyscript", "CONST_MODIFY", NULL, 
				"Attempt to modify const variable: $varname", 
				log_data_string("varname", clean_name));
			return;
		}
		/* Update existing variable */
		if (existing->value) free(existing->value);
		existing->value = value ? strdup(value) : NULL;
		return;
	}
	
	/* Create new variable */
	ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
	var->name = strdup(clean_name);
	var->value = value ? strdup(value) : NULL;
	var->type = US_VAR_STRING; /* Default to string type */
	var->object_ptr = NULL;
	var->is_const = is_const;
	var->next = scope->variables;
	scope->variables = var;
}

void set_variable_object(ObbyScriptScope *scope, const char *name, void *object_ptr, ObbyScriptVarType type, int is_const)
{
	if (!scope || !name)
		return;
		
	/* Remove % prefix if present */
	const char *clean_name = (name[0] == '%') ? name + 1 : name;
	
	/* Check if variable already exists */
	ObbyScriptVariable *existing = find_variable(scope, clean_name);
	if (existing)
	{
		if (existing->is_const)
		{
			unreal_log(ULOG_WARNING, "obbyscript", "CONST_MODIFY", NULL, 
				"Attempt to modify const variable: $varname", 
				log_data_string("varname", clean_name));
			return;
		}
		/* Update existing variable */
		if (existing->value) free(existing->value);
		existing->value = NULL;
		existing->type = type;
		existing->object_ptr = object_ptr;
		return;
	}
	
	/* Create new variable */
	ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
	var->name = strdup(clean_name);
	var->value = NULL;
	var->type = type;
	var->object_ptr = object_ptr;
	var->is_const = is_const;
	var->next = scope->variables;
	scope->variables = var;
}

char *get_variable(ObbyScriptScope *scope, const char *name)
{
	if (!scope || !name)
		return NULL;
		
	/* Remove % prefix if present */
	const char *clean_name = (name[0] == '%') ? name + 1 : name;
	
	ObbyScriptVariable *var = find_variable(scope, clean_name);
	return var ? var->value : NULL;
}

ObbyScriptVariable *get_variable_object(ObbyScriptScope *scope, const char *name)
{
	if (!scope || !name)
		return NULL;
		
	/* Remove % prefix if present */
	const char *clean_name = (name[0] == '%') ? name + 1 : name;
	
	return find_variable(scope, clean_name);
}

ObbyScriptVariable *find_variable(ObbyScriptScope *scope, const char *name)
{
	if (!scope || !name)
		return NULL;
		
	/* Remove % prefix if present */
	const char *clean_name = (name[0] == '%') ? name + 1 : name;
	
	/* Search current scope */
	ObbyScriptVariable *var = scope->variables;
	while (var)
	{
		if (strcmp(var->name, clean_name) == 0)
			return var;
		var = var->next;
	}
	
	/* Search parent scope if available */
	if (scope->parent)
		return find_variable(scope->parent, clean_name);
		
	return NULL;
}

void init_global_scope(void)
{
	if (global_scope)
		free_scope(global_scope);
	global_scope = create_scope(NULL);
	
	/* Initialize built-in variables */
	set_variable(global_scope, "true", "1", 1);   /* $true */
	set_variable(global_scope, "false", "0", 1);  /* $false */
	set_variable(global_scope, "null", "", 1);    /* $null */
}

void execute_start_events(void)
{
	if (!script_files)
		return;
		
	ObbyScriptFile *file;
	ObbyScriptRule *rule;
	
	for (file = script_files; file; file = file->next)
	{
		if (!file->rules)
			continue;
			
		for (rule = file->rules; rule; rule = rule->next)
		{
			if (rule->event == US_EVENT_START && rule->actions)
			{
				/* Execute START event actions */
				ObbyScriptAction *action = rule->actions;
				while (action)
				{
					execute_script_action(action, NULL, NULL);
					action = action->next;
				}
			}
		}
	}
}

/* Array management functions */
ObbyScriptArray *create_array(int initial_capacity)
{
	ObbyScriptArray *array = safe_alloc(sizeof(ObbyScriptArray));
	array->capacity = initial_capacity > 0 ? initial_capacity : 10;
	array->size = 0;
	array->elements = safe_alloc(sizeof(ObbyScriptArrayElement*) * array->capacity);
	return array;
}

void free_array(ObbyScriptArray *array)
{
	if (!array)
		return;
	
	for (int i = 0; i < array->size; i++)
	{
		if (array->elements[i])
		{
			if (array->elements[i]->string_value)
				free(array->elements[i]->string_value);
			free(array->elements[i]);
		}
	}
	
	free(array->elements);
	free(array);
}

void array_push_string(ObbyScriptArray *array, const char *value)
{
	if (!array)
		return;
	
	/* Resize if needed */
	if (array->size >= array->capacity)
	{
		array->capacity *= 2;
		array->elements = realloc(array->elements, sizeof(ObbyScriptArrayElement*) * array->capacity);
	}
	
	/* Create new element */
	ObbyScriptArrayElement *elem = safe_alloc(sizeof(ObbyScriptArrayElement));
	elem->type = US_VAR_STRING;
	elem->string_value = value ? strdup(value) : NULL;
	elem->object_ptr = NULL;
	elem->next = NULL;
	
	array->elements[array->size++] = elem;
}

void array_push_object(ObbyScriptArray *array, void *object_ptr, ObbyScriptVarType type)
{
	if (!array)
		return;
	
	/* Resize if needed */
	if (array->size >= array->capacity)
	{
		array->capacity *= 2;
		array->elements = realloc(array->elements, sizeof(ObbyScriptArrayElement*) * array->capacity);
	}
	
	/* Create new element */
	ObbyScriptArrayElement *elem = safe_alloc(sizeof(ObbyScriptArrayElement));
	elem->type = type;
	elem->string_value = NULL;
	elem->object_ptr = object_ptr;
	elem->next = NULL;
	
	array->elements[array->size++] = elem;
}

char *array_get_string(ObbyScriptArray *array, int index)
{
	if (!array || index < 0 || index >= array->size)
		return NULL;
	
	ObbyScriptArrayElement *elem = array->elements[index];
	if (!elem)
		return NULL;
	
	return elem->string_value;
}

void *array_get_object(ObbyScriptArray *array, int index, ObbyScriptVarType *out_type)
{
	if (!array || index < 0 || index >= array->size)
		return NULL;
	
	ObbyScriptArrayElement *elem = array->elements[index];
	if (!elem)
		return NULL;
	
	if (out_type)
		*out_type = elem->type;
	
	return elem->object_ptr;
}

void array_set_string(ObbyScriptArray *array, int index, const char *value)
{
	if (!array || index < 0)
		return;
	
	/* Expand array if needed */
	while (index >= array->capacity)
	{
		array->capacity *= 2;
		array->elements = realloc(array->elements, sizeof(ObbyScriptArrayElement*) * array->capacity);
	}
	
	/* Fill gaps with NULL if needed */
	while (array->size <= index)
	{
		array->elements[array->size++] = NULL;
	}
	
	/* Free existing element if present */
	if (array->elements[index])
	{
		if (array->elements[index]->string_value)
			free(array->elements[index]->string_value);
		free(array->elements[index]);
	}
	
	/* Create new element */
	ObbyScriptArrayElement *elem = safe_alloc(sizeof(ObbyScriptArrayElement));
	elem->type = US_VAR_STRING;
	elem->string_value = value ? strdup(value) : NULL;
	elem->object_ptr = NULL;
	elem->next = NULL;
	
	array->elements[index] = elem;
}

void array_set_object(ObbyScriptArray *array, int index, void *object_ptr, ObbyScriptVarType type)
{
	if (!array || index < 0)
		return;
	
	/* Expand array if needed */
	while (index >= array->capacity)
	{
		array->capacity *= 2;
		array->elements = realloc(array->elements, sizeof(ObbyScriptArrayElement*) * array->capacity);
	}
	
	/* Fill gaps with NULL if needed */
	while (array->size <= index)
	{
		array->elements[array->size++] = NULL;
	}
	
	/* Free existing element if present */
	if (array->elements[index])
	{
		if (array->elements[index]->string_value)
			free(array->elements[index]->string_value);
		free(array->elements[index]);
	}
	
	/* Create new element */
	ObbyScriptArrayElement *elem = safe_alloc(sizeof(ObbyScriptArrayElement));
	elem->type = type;
	elem->string_value = NULL;
	elem->object_ptr = object_ptr;
	elem->next = NULL;
	
	array->elements[index] = elem;
}

ObbyScriptArray *get_client_channels(Client *client)
{
	if (!client || !IsUser(client) || !client->user)
		return NULL;
	
	ObbyScriptArray *array = create_array(10);
	Membership *membership;
	
	for (membership = client->user->channel; membership; membership = membership->next)
	{
		if (membership->channel && membership->channel->name)
		{
			array_push_string(array, membership->channel->name);
		}
	}
	
	return array;
}

void set_variable_array(ObbyScriptScope *scope, const char *name, ObbyScriptArray *array, int is_const)
{
	if (!scope || !name)
		return;
	
	/* Remove % prefix if present */
	const char *clean_name = (name[0] == '%') ? name + 1 : name;
	
	/* Check if variable already exists */
	ObbyScriptVariable *existing = find_variable(scope, clean_name);
	if (existing)
	{
		if (existing->is_const)
		{
			unreal_log(ULOG_WARNING, "obbyscript", "CONST_MODIFY", NULL,
				"Attempt to modify const variable: $varname",
				log_data_string("varname", clean_name));
			return;
		}
		/* Update existing variable */
		if (existing->value) free(existing->value);
		if (existing->array_ptr) free_array(existing->array_ptr);
		existing->value = NULL;
		existing->type = US_VAR_ARRAY;
		existing->object_ptr = NULL;
		existing->array_ptr = array;
		return;
	}
	
	/* Create new variable */
	ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
	var->name = strdup(clean_name);
	var->value = NULL;
	var->type = US_VAR_ARRAY;
	var->object_ptr = NULL;
	var->array_ptr = array;
	var->is_const = is_const;
	var->next = scope->variables;
	scope->variables = var;
}

ObbyScriptArray *parse_array_literal(const char *array_str, Client *client, Channel *channel)
{
	if (!array_str)
		return NULL;
	
	/* Skip whitespace and check for opening bracket */
	const char *p = array_str;
	while (*p && isspace(*p)) p++;
	
	if (*p != '[')
		return NULL;
	p++;
	
	/* Create array */
	ObbyScriptArray *array = create_array(10);
	
	/* Parse elements */
	while (*p && *p != ']')
	{
		/* Skip whitespace */
		while (*p && isspace(*p)) p++;
		
		if (*p == ']')
			break;
		
		/* Check for comma separator */
		if (*p == ',')
		{
			p++;
			continue;
		}
		
		/* Parse element */
		if (*p == '"')
		{
			/* String literal */
			p++;
			const char *start = p;
			while (*p && *p != '"')
			{
				if (*p == '\\' && *(p+1))
					p++; /* Skip escaped character */
				p++;
			}
			
			int len = p - start;
			char *str = safe_alloc(len + 1);
			strncpy(str, start, len);
			str[len] = '\0';
			
			array_push_string(array, str);
			free(str);
			
			if (*p == '"') p++;
		}
		else if (*p == '$')
		{
			/* Variable or special object reference */
			const char *start = p;
			p++;
			while (*p && (isalnum(*p) || *p == '_' || *p == '.')) p++;
			
			int len = p - start;
			char *var_name = safe_alloc(len + 1);
			strncpy(var_name, start, len);
			var_name[len] = '\0';
			
			/* Substitute the variable */
			char *substituted = substitute_variables(var_name, client, channel);
			if (substituted)
			{
				/* Check if it's an object reference */
				if (strcmp(var_name, "$client") == 0 && client)
				{
					array_push_object(array, client, US_VAR_CLIENT);
				}
				else if (strcmp(var_name, "$chan") == 0 && channel)
				{
					array_push_object(array, channel, US_VAR_CHANNEL);
				}
				else
				{
					array_push_string(array, substituted);
				}
				free(substituted);
			}
			else
			{
				array_push_string(array, "$null");
			}
			
			free(var_name);
		}
		else if (*p == '%')
		{
			/* User variable reference */
			const char *start = p;
			p++;
			while (*p && (isalnum(*p) || *p == '_')) p++;
			
			int len = p - start;
			char *var_name = safe_alloc(len + 1);
			strncpy(var_name, start, len);
			var_name[len] = '\0';
			
			/* Get variable value */
			ObbyScriptVariable *var = get_variable_object(global_scope, var_name);
			if (var)
			{
				if (var->type == US_VAR_STRING && var->value)
				{
					array_push_string(array, var->value);
				}
				else if (var->type == US_VAR_CLIENT || var->type == US_VAR_CHANNEL)
				{
					array_push_object(array, var->object_ptr, var->type);
				}
				else
				{
					array_push_string(array, "$null");
				}
			}
			else
			{
				array_push_string(array, "$null");
			}
			
			free(var_name);
		}
		else
		{
			/* Literal value without quotes */
			const char *start = p;
			while (*p && *p != ',' && *p != ']' && !isspace(*p)) p++;
			
			int len = p - start;
			char *str = safe_alloc(len + 1);
			strncpy(str, start, len);
			str[len] = '\0';
			
			array_push_string(array, str);
			free(str);
		}
	}
	
	return array;
}

/* CAP capability management functions */
void add_pending_cap(const char *cap_name)
{
	ObbyScriptCapability *cap;
	
	if (!cap_name || !*cap_name)
		return;
	
	/* Check if already exists */
	for (cap = pending_caps; cap; cap = cap->next)
	{
		if (!strcmp(cap->name, cap_name))
			return; /* Already exists */
	}
	
	/* Add new capability */
	cap = safe_alloc(sizeof(ObbyScriptCapability));
	cap->name = strdup(cap_name);
	cap->next = pending_caps;
	pending_caps = cap;
}

void register_pending_caps(void)
{
	ObbyScriptCapability *cap;
	
	for (cap = pending_caps; cap; cap = cap->next)
	{
		/* Create ClientCapabilityInfo structure */
		ClientCapabilityInfo clicap_info;
		memset(&clicap_info, 0, sizeof(clicap_info));
		clicap_info.name = cap->name;
		clicap_info.flags = 0;
		clicap_info.visible = NULL;  /* Always visible */
		clicap_info.parameter = NULL;  /* No parameter */
		
		/* Register the capability with UnrealIRCd */
		ClientCapability *clicap = ClientCapabilityAdd(obbyscript_module_handle, &clicap_info, NULL);
		if (clicap)
		{
			unreal_log(ULOG_DEBUG, "obbyscript", "CAP_REGISTERED", NULL, 
				"Successfully registered CAP capability: $cap", 
				log_data_string("cap", cap->name));
		}
		else
		{
			unreal_log(ULOG_WARNING, "obbyscript", "CAP_REGISTER_FAILED", NULL, 
				"Failed to register CAP capability: $cap", 
				log_data_string("cap", cap->name));
		}
	}
}

/* Arithmetic evaluation functions */
int is_arithmetic_operation(const char *line)
{
	fprintf(stderr, "[ARITH_CHECK] Checking '%s'\n", line);
	/* Check for variable increment/decrement patterns */
	if (line[0] == '%' && (strstr(line, "++") || strstr(line, "--") || strstr(line, "+=") || strstr(line, "-=") || strstr(line, "*=") || strstr(line, "/=")))
	{
		fprintf(stderr, "[ARITH_CHECK] YES - matches increment/decrement pattern\n");
		return 1;
	}
	
	/* Check for assignment with arithmetic */
	if (line[0] == '%' && strchr(line, '=') && (strchr(line, '+') || strchr(line, '-') || strchr(line, '*') || strchr(line, '/')))
	{
		fprintf(stderr, "[ARITH_CHECK] YES - matches arithmetic assignment\n");
		return 1;
	}
	
	fprintf(stderr, "[ARITH_CHECK] NO - not arithmetic\n");
	return 0;
}

int evaluate_arithmetic(const char *expression, Client *client, Channel *channel)
{
	char *expr = substitute_variables(expression, client, channel);
	if (!expr) return 0;
	
	/* Simple arithmetic parser for basic operations */
	char *p = expr;
	int result = 0;
	int current_num = 0;
	char operator = '+';
	int first_number = 1;
	
	/* Skip whitespace */
	while (*p && isspace(*p)) p++;
	
	/* Handle negative numbers at start */
	if (*p == '-')
	{
		operator = '-';
		p++;
		first_number = 1;
	}
	
	while (*p)
	{
		if (isdigit(*p))
		{
			current_num = current_num * 10 + (*p - '0');
		}
		else if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '\0')
		{
			/* Apply previous operation */
			if (first_number)
			{
				result = (operator == '-') ? -current_num : current_num;
				first_number = 0;
			}
			else
			{
				switch (operator)
				{
					case '+': result += current_num; break;
					case '-': result -= current_num; break;
					case '*': result *= current_num; break;
					case '/': if (current_num != 0) result /= current_num; break;
				}
			}
			
			operator = *p;
			current_num = 0;
		}
		else if (!isspace(*p))
		{
			/* Invalid character, return current result */
			break;
		}
		
		if (*p) p++;
	}
	
	/* Apply final operation if needed */
	if (current_num > 0 || operator != '\0')
	{
		if (first_number)
		{
			result = (operator == '-') ? -current_num : current_num;
		}
		else
		{
			switch (operator)
			{
				case '+': result += current_num; break;
				case '-': result -= current_num; break;
				case '*': result *= current_num; break;
				case '/': if (current_num != 0) result /= current_num; break;
			}
		}
	}
	
	free(expr);
	return result;
}

/* Function management functions */
void add_function(const char *name, char **parameters, int param_count, ObbyScriptAction *body)
{
	if (!name || !body)
		return;

	/* Log that add_function was called (info level so it appears in logs) */
	unreal_log(ULOG_DEBUG, "obbyscript", "FUNCTION_ADD_ATTEMPT", NULL,
		"Attempting to add function '$name' with $params parameters",
		log_data_string("name", name),
		log_data_integer("params", param_count));
	
	/* Check if function already exists */
	ObbyScriptFunction *existing = find_function(name);
	if (existing)
	{
		unreal_log(ULOG_WARNING, "obbyscript", "FUNCTION_REDEFINED", NULL,
			"Function '$name' redefined", 
			log_data_string("name", name));
		return;
	}
	
	/* Create new function */
	ObbyScriptFunction *func = safe_alloc(sizeof(ObbyScriptFunction));
	func->name = strdup(name);
	func->param_count = param_count;
	func->body = body;
	
	if (param_count > 0 && parameters)
	{
		func->parameters = safe_alloc(sizeof(char*) * param_count);
		for (int i = 0; i < param_count; i++)
			func->parameters[i] = strdup(parameters[i]);
	}
	
	/* Add to global functions list */
	func->next = global_functions;
	global_functions = func;
	
	unreal_log(ULOG_DEBUG, "obbyscript", "FUNCTION_DEFINED", NULL,
		"Function '$name' defined with $params parameters", 
		log_data_string("name", name),
		log_data_integer("params", param_count));
}

ObbyScriptFunction *find_function(const char *name)
{
	if (!name)
		return NULL;
	
	ObbyScriptFunction *func = global_functions;
	while (func)
	{
		if (strcmp(func->name, name) == 0)
			return func;
		func = func->next;
	}
	
	return NULL;
}

void free_function(ObbyScriptFunction *func)
{
	if (!func)
		return;
	
	free(func->name);
	
	if (func->parameters)
	{
		for (int i = 0; i < func->param_count; i++)
			free(func->parameters[i]);
		free(func->parameters);
	}
	
	if (func->body)
		free_script_action(func->body);
	
	free(func);
}

void free_all_functions(void)
{
	ObbyScriptFunction *func = global_functions;
	while (func)
	{
		ObbyScriptFunction *next = func->next;
		free_function(func);
		func = next;
	}
	global_functions = NULL;
}

int execute_function(const char *name, char **args, int arg_count, Client *client, Channel *channel, char **return_value)
{
	/* Check if it's a built-in function first */
	if (is_builtin_function(name))
	{
		ObbyScriptVariable *result = execute_builtin_function(name, args, arg_count);
		if (result)
		{
			if (return_value)
			{
				if (result->type == US_VAR_STRING && result->value)
				{
					*return_value = strdup(result->value);
				}
				else
				{
					/* For object types, return a special string indicating the object type */
					if (result->type == US_VAR_CLIENT)
						*return_value = strdup("__CLIENT_OBJECT__");
					else if (result->type == US_VAR_CHANNEL)
						*return_value = strdup("__CHANNEL_OBJECT__");
					else
						*return_value = strdup("");
				}
			}
			/* Don't free result here - it might be stored in global scope */
			return 1;
		}
		return 0;
	}

	ObbyScriptFunction *func = find_function(name);
	if (!func)
	{
		unreal_log(ULOG_WARNING, "obbyscript", "FUNCTION_NOT_FOUND", NULL,
			"Function '$name' not found", 
			log_data_string("name", name));
		return 0;
	}
	
	/* Check parameter count */
	if (arg_count != func->param_count)
	{
		unreal_log(ULOG_WARNING, "obbyscript", "FUNCTION_PARAM_MISMATCH", NULL,
			"Function '$name' expects $expected parameters but got $actual", 
			log_data_string("name", name),
			log_data_integer("expected", func->param_count),
			log_data_integer("actual", arg_count));
		return 0;
	}
	
	/* Create function scope with parameters */
	ObbyScriptScope *func_scope = create_scope(global_scope);
	
	/* Set parameter variables */
	for (int i = 0; i < func->param_count; i++)
	{
		if (func->parameters[i] && args[i])
			set_variable(func_scope, func->parameters[i], args[i], 0);
	}
	
	/* Save current global scope and switch to function scope */
	ObbyScriptScope *saved_scope = global_scope;
	global_scope = func_scope;
	
	/* Execute function body with original context but function scope variables */
	if (func->body)
		execute_script_action(func->body, client, channel);
	
	/* Check for return value in function scope */
	if (return_value)
	{
		char *ret_val = get_variable(func_scope, "__return__");
		if (ret_val)
			*return_value = strdup(ret_val);
		else
			*return_value = NULL;
	}
	
	/* Restore global scope */
	global_scope = saved_scope;
	
	/* Free function scope */
	free_scope(func_scope);
	
	return 1;
}

int execute_function_with_objects(const char *name, char **args, ObbyScriptVariable **object_args, int arg_count, Client *client, Channel *channel, char **return_value)
{
	/* Check if it's a built-in function first */
	if (is_builtin_function(name))
	{
		/* For built-in functions, use the regular execution path */
		return execute_function(name, args, arg_count, client, channel, return_value);
	}

	ObbyScriptFunction *func = find_function(name);
	if (!func)
	{
		unreal_log(ULOG_WARNING, "obbyscript", "FUNCTION_NOT_FOUND", NULL,
			"Function '$name' not found", 
			log_data_string("name", name));
		return 0;
	}
	
	/* Check parameter count */
	if (arg_count != func->param_count)
	{
		unreal_log(ULOG_WARNING, "obbyscript", "FUNCTION_PARAM_MISMATCH", NULL,
			"Function '$name' expects $expected parameters but got $actual", 
			log_data_string("name", name),
			log_data_integer("expected", func->param_count),
			log_data_integer("actual", arg_count));
		return 0;
	}
	
	/* Create function scope with parameters */
	ObbyScriptScope *func_scope = create_scope(global_scope);
	
	/* Set parameter variables - handle objects specially */
	for (int i = 0; i < func->param_count; i++)
	{
		if (func->parameters[i])
		{
			if (object_args && object_args[i])
			{
				/* This is an object parameter - set it as an object variable */
				set_variable_object(func_scope, func->parameters[i], object_args[i]->object_ptr, object_args[i]->type, 0);
			}
			else if (args[i])
			{
				/* Regular string parameter */
				set_variable(func_scope, func->parameters[i], args[i], 0);
			}
		}
	}
	
	/* Save current global scope and switch to function scope */
	ObbyScriptScope *saved_scope = global_scope;
	global_scope = func_scope;
	
	/* Execute function body with original context but function scope variables */
	if (func->body)
		execute_script_action(func->body, client, channel);
	
	/* Check for return value in function scope */
	if (return_value)
	{
		char *ret_val = get_variable(func_scope, "__return__");
		if (ret_val)
			*return_value = strdup(ret_val);
		else
			*return_value = NULL;
	}
	
	/* Restore global scope */
	global_scope = saved_scope;
	
	/* Free function scope */
	free_scope(func_scope);
	
	return 1;
}

int is_function_call(const char *line)
{
	if (!line)
		return 0;
	
	unreal_log(ULOG_DEBUG, "obbyscript", "IS_FUNCTION_CALL_DEBUG", NULL,
		"is_function_call called with: '$line'",
		log_data_string("line", line));
	
	/* Skip whitespace */
	while (*line && isspace(*line)) line++;
	
	/* Special case: if line starts with "var %name = function_call", extract the function call */
	if (strncmp(line, "var ", 4) == 0)
	{
		const char *equals = strchr(line, '=');
		if (equals)
		{
			equals++; /* Skip the = */
			while (*equals && isspace(*equals)) equals++; /* Skip whitespace after = */
			return is_function_call(equals); /* Recursively check the part after = */
		}
	}
	
	/* Check for $function_name( pattern (user-defined functions) */
	if (*line == '$')
	{
		const char *p = line + 1;
		while (*p && *p != '(' && !isspace(*p))
			p++;
		return (*p == '(');
	}
	
	/* Check for function_name( pattern (built-in functions) */
	const char *p = line;
	while (*p && *p != '(' && !isspace(*p))
		p++;
	
	if (*p == '(')
	{
		/* Extract function name and check if it's a built-in */
		int name_len = p - line;
		char *func_name = safe_alloc(name_len + 1);
		strncpy(func_name, line, name_len);
		func_name[name_len] = '\0';
		
		int is_builtin = is_builtin_function(func_name);
		free(func_name);
		return is_builtin;
	}
	
	return 0;
}

/* Built-in function implementations */
int is_builtin_function(const char *name)
{
	if (!name)
		return 0;
	
	return (strcmp(name, "find_client") == 0 ||
	        strcmp(name, "find_server") == 0 ||
	        strcmp(name, "find_channel") == 0);
}

ObbyScriptVariable *execute_builtin_function(const char *name, char **args, int arg_count)
{
	if (strcmp(name, "find_client") == 0)
		return obbyscript_find_client(args, arg_count);
	else if (strcmp(name, "find_server") == 0)
		return obbyscript_find_server(args, arg_count);
	else if (strcmp(name, "find_channel") == 0)
		return obbyscript_find_channel(args, arg_count);
	
	return NULL;
}

ObbyScriptVariable *obbyscript_find_client(char **args, int arg_count)
{
	unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_FIND_CLIENT", NULL,
		"find_client called with $argc args",
		log_data_integer("argc", arg_count));
	
	if (arg_count != 1 || !args[0])
	{
		unreal_log(ULOG_WARNING, "obbyscript", "DEBUG_FIND_CLIENT_ARGS", NULL,
			"find_client: invalid arguments");
		return NULL;
	}
	
	unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_FIND_CLIENT_SEARCH", NULL,
		"Searching for client: $nick",
		log_data_string("nick", args[0]));
	
	Client *client = find_client(args[0], NULL);
	if (!client)
	{
		unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_FIND_CLIENT_NOTFOUND", NULL,
			"Client $nick not found",
			log_data_string("nick", args[0]));
		
		/* Return a variable with $false value to indicate not found */
		ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
		var->name = strdup("__temp_client__");
		var->value = strdup("$false");
		var->type = US_VAR_STRING;
		var->object_ptr = NULL;
		var->is_const = 0;
		var->next = NULL;
		
		return var;
	}
	
	unreal_log(ULOG_DEBUG, "obbyscript", "DEBUG_FIND_CLIENT_FOUND", NULL,
		"Found client: $nick",
		log_data_string("nick", client->name));
	
	/* Create a variable to store the client object */
	ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
	var->name = strdup("__temp_client__");
	var->value = NULL;
	var->type = US_VAR_CLIENT;
	var->object_ptr = client;
	var->is_const = 0;
	var->next = NULL;
	
	return var;
}

ObbyScriptVariable *obbyscript_find_server(char **args, int arg_count)
{
	if (arg_count != 1 || !args[0])
		return NULL;
	
	Client *server = find_server(args[0], NULL);
	if (!server)
	{
		/* Return a variable with $false value to indicate not found */
		ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
		var->name = strdup("__temp_server__");
		var->value = strdup("$false");
		var->type = US_VAR_STRING;
		var->object_ptr = NULL;
		var->is_const = 0;
		var->next = NULL;
		
		return var;
	}
	
	/* Create a variable to store the server object */
	ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
	var->name = strdup("__temp_server__");
	var->value = NULL;
	var->type = US_VAR_CLIENT; /* Servers are also Client objects in UnrealIRCd */
	var->object_ptr = server;
	var->is_const = 0;
	var->next = NULL;
	
	return var;
}

ObbyScriptVariable *obbyscript_find_channel(char **args, int arg_count)
{
	if (arg_count != 1 || !args[0])
		return NULL;
	
	Channel *channel = find_channel(args[0]);
	if (!channel)
	{
		/* Return a variable with $false value to indicate not found */
		ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
		var->name = strdup("__temp_channel__");
		var->value = strdup("$false");
		var->type = US_VAR_STRING;
		var->object_ptr = NULL;
		var->is_const = 0;
		var->next = NULL;
		
		return var;
	}
	
	/* Create a variable to store the channel object */
	ObbyScriptVariable *var = safe_alloc(sizeof(ObbyScriptVariable));
	var->name = strdup("__temp_channel__");
	var->value = NULL;
	var->type = US_VAR_CHANNEL;
	var->object_ptr = channel;
	var->is_const = 0;
	var->next = NULL;
	
	return var;
}

char *evaluate_condition_value(const char *condition, Client *client, Channel *channel)
{
	if (!condition)
		return NULL;
	
	unreal_log(ULOG_DEBUG, "obbyscript", "EVALUATE_CONDITION_DEBUG", NULL,
		"evaluate_condition_value called with: '$condition'",
		log_data_string("condition", condition));
	
	/* Check if this is a function call first */
	if (is_function_call(condition))
	{
		unreal_log(ULOG_DEBUG, "obbyscript", "FUNCTION_CALL_EVAL", NULL,
			"Evaluating function call in condition: $condition",
			log_data_string("condition", condition));
			
		/* Parse and execute the function call */
		ObbyScriptAction *temp_action = safe_alloc(sizeof(ObbyScriptAction));
		temp_action->type = US_ACTION_FUNCTION_CALL;
		
		/* Parse function call */
		char *line_copy = strdup(condition);
		char *func_name_start = line_copy;
		
		/* Skip $ if present */
		if (*func_name_start == '$')
			func_name_start++;
		
		/* Find opening parenthesis */
		char *paren = strchr(func_name_start, '(');
		if (paren)
		{
			*paren = '\0'; /* Terminate function name */
			safe_strdup(temp_action->function, func_name_start);
			
			/* Parse arguments */
			char *args_start = paren + 1;
			char *args_end = strrchr(args_start, ')');
			if (args_end)
			{
				*args_end = '\0'; /* Terminate arguments */
				
				/* Split arguments by comma */
				temp_action->argc = 0;
				if (strlen(args_start) > 0)
				{
					/* Count arguments */
					char *arg_copy = strdup(args_start);
					char *token = strtok(arg_copy, ",");
					while (token)
					{
						temp_action->argc++;
						token = strtok(NULL, ",");
					}
					free(arg_copy);
					
					/* Allocate and store arguments */
					if (temp_action->argc > 0)
					{
						temp_action->args = safe_alloc(sizeof(char*) * temp_action->argc);
						arg_copy = strdup(args_start);
						token = strtok(arg_copy, ",");
						int i = 0;
						while (token && i < temp_action->argc)
						{
							/* Trim whitespace */
							while (*token && isspace(*token)) token++;
							char *end = token + strlen(token) - 1;
							while (end > token && isspace(*end)) *end-- = '\0';
							
							temp_action->args[i] = strdup(token);
							token = strtok(NULL, ",");
							i++;
						}
						free(arg_copy);
					}
				}
				
				/* Execute the function call with object-aware parameter passing */
				char **substituted_args = NULL;
				ObbyScriptVariable **object_args = NULL;
				if (temp_action->argc > 0)
				{
					substituted_args = safe_alloc(sizeof(char*) * temp_action->argc);
					object_args = safe_alloc(sizeof(ObbyScriptVariable*) * temp_action->argc);
					for (int i = 0; i < temp_action->argc; i++)
					{
						object_args[i] = NULL;
						
						/* Check if argument is an object variable (starts with %) */
						if (temp_action->args[i] && temp_action->args[i][0] == '%')
						{
							/* Extract variable name (remove % prefix) */
							char *var_name = temp_action->args[i] + 1;
							ObbyScriptVariable *var = get_variable_object(global_scope, var_name);
							if (var && (var->type == US_VAR_CLIENT || var->type == US_VAR_CHANNEL))
							{
								/* This is an object variable - pass it directly */
								object_args[i] = var;
								substituted_args[i] = strdup("__OBJECT__"); /* Placeholder */
								continue;
							}
						}
						
						/* Regular variable substitution */
						substituted_args[i] = substitute_variables(temp_action->args[i], client, channel);
						if (!substituted_args[i])
							substituted_args[i] = strdup(temp_action->args[i]);
					}
				}
				
				/* Execute the function and get return value */
				char *return_value = NULL;
				execute_function_with_objects(temp_action->function, substituted_args, object_args, temp_action->argc, client, channel, &return_value);
				
				unreal_log(ULOG_DEBUG, "obbyscript", "FUNCTION_CALL_RESULT", NULL,
					"Function call result: $result",
					log_data_string("result", return_value ? return_value : "NULL"));
				
				/* Free memory */
				if (substituted_args)
				{
					for (int i = 0; i < temp_action->argc; i++)
						free(substituted_args[i]);
					free(substituted_args);
				}
				if (object_args)
					free(object_args);
				
				if (temp_action->args)
				{
					for (int i = 0; i < temp_action->argc; i++)
						free(temp_action->args[i]);
					free(temp_action->args);
				}
				if (temp_action->function)
					free(temp_action->function);
				free(temp_action);
				free(line_copy);
				
				return return_value ? return_value : strdup("");
			}
		}
		
		/* Function call parsing failed */
		if (temp_action->function)
			free(temp_action->function);
		free(temp_action);
		free(line_copy);
	}
	
	/* Substitute variables */
	char *substituted = substitute_variables(condition, client, channel);
	if (!substituted)
		return strdup(condition);
	
	return substituted;
}

int is_falsy_value(const char *value)
{
	if (!value)
		return 1; /* NULL is falsy */
	
	/* Empty string is falsy */
	if (strlen(value) == 0)
		return 1;
	
	/* "0" is falsy */
	if (strcmp(value, "0") == 0)
		return 1;
	
	/* "$false" and "false" are falsy */
	if (strcmp(value, "$false") == 0 || strcmp(value, "false") == 0)
		return 1;
	
	/* "$null" and "null" are falsy */
	if (strcmp(value, "$null") == 0 || strcmp(value, "null") == 0)
		return 1;
	
	/* Everything else is truthy */
	return 0;
}

/* Command management functions */
void register_commands_for_file(ObbyScriptFile *file)
{
	ObbyScriptRule *rule;
	
	if (!file || !file->rules)
		return;
	
	/* Scan rules in this file for command rules */
	for (rule = file->rules; rule; rule = rule->next)
	{
		if (rule->event == US_EVENT_COMMAND_OVERRIDE)
		{
			/* Register command override: on COMMAND:CMDNAME */
			ObbyScriptCommand *cmd = safe_alloc(sizeof(ObbyScriptCommand));
			cmd->command = strdup(rule->target);
			cmd->rule = rule;
			cmd->ovr_ptr = CommandOverrideAdd(obbyscript_module_handle, rule->target, 0, obbyscript_command_override_handler);
			
			/* Add to our tracking list */
			cmd->next = registered_commands;
			registered_commands = cmd;
		}
		else if (rule->event == US_EVENT_COMMAND_NEW)
		{
			/* Register new command: new COMMAND:CMDNAME */
			ObbyScriptCommand *cmd = safe_alloc(sizeof(ObbyScriptCommand));
			cmd->command = strdup(rule->target);
			cmd->rule = rule;
			cmd->cmd_ptr = CommandAdd(obbyscript_module_handle, rule->target, obbyscript_command_handler, MAXPARA, CMD_USER);
			
			/* Add to our tracking list */
			cmd->next = registered_commands;
			registered_commands = cmd;
		}
	}
}

void register_script_commands(void)
{
	ObbyScriptFile *file;
	
	/* Scan all script files for command rules */
	for (file = script_files; file; file = file->next)
	{
		register_commands_for_file(file);
	}
}

void unregister_script_commands(void)
{
	ObbyScriptCommand *cmd, *next;
	
	for (cmd = registered_commands; cmd; cmd = next)
	{
		next = cmd->next;
		
		if (cmd->command)
			safe_free(cmd->command);
		
		/* Commands and overrides are automatically unregistered when module unloads */
		safe_free(cmd);
	}
	
	registered_commands = NULL;
}

CMD_FUNC(obbyscript_command_handler)
{
	ObbyScriptCommand *cmd;
	
	/* Sanity check */
	if (!registered_commands || parc <= 0 || !parv || !parv[0])
		return;
	
	/* Find the command in our registered list by checking parv[0] */
	for (cmd = registered_commands; cmd; cmd = cmd->next)
	{
		/* Safety checks to prevent crashes */
		if (!cmd->cmd_ptr || !cmd->command)
			continue;
			
		if (strcasecmp(cmd->command, clictx->cmd->cmd) == 0)
		{
			/* Execute the script actions for this command with parameter substitution */
			if (cmd->rule && cmd->rule->actions)
			{
				execute_script_action_with_params(cmd->rule->actions, client, NULL, parc, parv);
			}
			return;
		}
	}
}

CMD_OVERRIDE_FUNC(obbyscript_command_override_handler)
{
	ObbyScriptCommand *cmd;
	
	/* Sanity check */
	if (!registered_commands || !ovr)
	{
		CALL_NEXT_COMMAND_OVERRIDE();
		return;
	}
	
	/* Find the command override in our registered list */
	for (cmd = registered_commands; cmd; cmd = cmd->next)
	{
		if (cmd->ovr_ptr == ovr)
		{
			/* Execute the script actions for this command override with parameter substitution */
			if (cmd->rule && cmd->rule->actions)
			{
				execute_script_action_with_params(cmd->rule->actions, client, NULL, parc, parv);
			}
			
			/* Call the next command override */
			CALL_NEXT_COMMAND_OVERRIDE();
			return;
		}
	}
	
	/* Fallback - call next override */
	CALL_NEXT_COMMAND_OVERRIDE();
}

char *substitute_command_parameters(const char *text, int parc, const char *parv[], Client *client, Channel *channel)
{
	char *result = strdup(text);
	char *temp;
	char param_name[16];
	char param_value[512];
	
	if (!result)
		return NULL;
	
	/* Debug: Log parameter substitution details */
	unreal_log(ULOG_DEBUG, "obbyscript", "PARAM_SUBSTITUTE_DEBUG", client,
		"Parameter substitution called for command context");
	
	/* Handle $1-3, $2-5, etc. (ranges of parameters) FIRST to avoid conflicts with individual parameter substitution */
	for (int start = 1; start <= MAXPARA; start++)
	{
		for (int end = start + 1; end <= MAXPARA; end++)
		{
			snprintf(param_name, sizeof(param_name), "$%d-%d", start, end);
			
			/* Build the range value */
			param_value[0] = '\0';
			for (int i = start; i <= end && i < parc; i++)
			{
				if (parv[i])
				{
					if (param_value[0] != '\0')
						strlcat(param_value, " ", sizeof(param_value));
					strlcat(param_value, parv[i], sizeof(param_value));
				}
			}
			
			/* Remove trailing whitespace */
			char *end_ptr = param_value + strlen(param_value) - 1;
			while (end_ptr > param_value && isspace(*end_ptr))
				*end_ptr-- = '\0';
				
			temp = obbyscript_replace_string(result, param_name, param_value);
			if (temp)
			{
				free(result);
				result = temp;
			}
		}
	}
	
	/* Handle $1-, $2-, etc. (unbounded ranges - everything from parameter N onwards) */
	for (int start = 1; start <= MAXPARA; start++)
	{
		snprintf(param_name, sizeof(param_name), "$%d-", start);
		
		/* Build the unbounded range value */
		param_value[0] = '\0';
		for (int i = start; i < parc; i++)
		{
			if (parv[i])
			{
				if (param_value[0] != '\0')
					strlcat(param_value, " ", sizeof(param_value));
				strlcat(param_value, parv[i], sizeof(param_value));
			}
		}
		
		/* Remove trailing whitespace */
		char *end_ptr = param_value + strlen(param_value) - 1;
		while (end_ptr > param_value && isspace(*end_ptr))
			*end_ptr-- = '\0';
			
		temp = obbyscript_replace_string(result, param_name, param_value);
		if (temp)
		{
			free(result);
			result = temp;
		}
	}
	
	/* Handle $1, $2, $3, etc. (parv[1], parv[2], parv[3], etc.) AFTER ranges */
	/* Process ALL potential parameters 1-MAXPARA, not just those provided */
	for (int i = 1; i <= MAXPARA; i++)
	{
		snprintf(param_name, sizeof(param_name), "$%d", i);
		if (i < parc && parv[i])
		{
			strlcpy(param_value, parv[i], sizeof(param_value));
		}
		else
		{
			strcpy(param_value, "$null");
		}
		
		temp = obbyscript_replace_string(result, param_name, param_value);
		if (temp)
		{
			free(result);
			result = temp;
		}
	}
	
	return result;
}
