/* Copyright © 2025 Valware & ObsidianIRC Team
 * License: GPLv3
 * Name: third/obsidian-register
 */
/*** <<<MODULE MANAGER START>>>
module
{
        documentation "https://github.com/link";
        troubleshooting "In case of problems, check the documentation or e-mail me at v.a.pond@outlook.com";
        min-unrealircd-version "6.1.0";
        max-unrealircd-version "6.*";
        post-install-text {
                "The module is installed. Now all you need to do is add a loadmodule line:";
                "loadmodule \"third/obsidian-register\";";
                "The module needs no other configuration.";
                "Once you're good to go, you can finally type in your shell: ./unrealircd rehash";
        }
}
*** <<<MODULE MANAGER END>>>

*/

/* One include for all */
#include "unrealircd.h"
#include "obsidian.h"

/**
 * my_find_tkl_nameban - Checks if a given name is banned via TKL nameban.
 * Returns a pointer to the TKL if found, otherwise NULL.
 */
TKL *my_find_tkl_nameban(const char *name)
{
	TKL *tkl;

	for (tkl = tklines[tkl_hash('Q')]; tkl; tkl = tkl->next)
	{
		if (!TKLIsNameBan(tkl))
			continue;
		if (!strcasecmp(name, tkl->ptr.nameban->name))
			return tkl;
	}
	return NULL;
}

#define CMD_REGISTER "REGISTER"
#define REGCAP_NAME "draft/account-registration"
/* Forward declarations */
CMD_FUNC(register_account);

const char *accreg_capability_parameter(Client *client);
int accreg_capability_visible(Client *client);

long CAP_ACCOUNTREGISTRATION = 0L;

ModuleHeader MOD_HEADER
={
    "third/o-register", /* Name of module */
    "1.0.0", /* Version */
    "Provides account registration", /* Short description of module */
    "ObsidianIRC Team", /* Author */
    "unrealircd-6", /* Version of UnrealIRCd */
};

// Create a new metadata node
Metadata* create_metadata(const char *key, const char *value) {
    Metadata *m = safe_alloc(sizeof(Metadata));
    m->key = strdup(key);
    m->value = strdup(value);
    m->prev = m->next = NULL;
    return m;
}

/**
 * add_metadata - Adds a metadata key/value pair to an Account.
 */
void add_metadata(Account *acc, const char *key, const char *value) {
    Metadata *m = create_metadata(key, value);
    m->next = acc->metadata_head;
    if (acc->metadata_head)
        acc->metadata_head->prev = m;
    acc->metadata_head = m;
}

/**
 * free_metadata - Frees a linked list of Metadata nodes.
 */
void free_metadata(Metadata *head) {
    Metadata *cur = head, *tmp;
    while (cur) {
        tmp = cur->next;
        free(cur->key);
        free(cur->value);
        free(cur);
        cur = tmp;
    }
}

/**
 * free_account - Frees all memory associated with an Account struct.
 */
void free_account(Account *acc) {
    if (!acc) return;
    free(acc->name);
    free(acc->email);
    free(acc->password);
    if (acc->channels) {
        for (char **c = acc->channels; *c; ++c)
            free(*c);
        free(acc->channels);
    }
    free_metadata(acc->metadata_head);
    free(acc);
}

/**
 * account_to_json - Converts an Account struct to a JSON object.
 * Returns a new json_t* object.
 */
json_t* account_to_json(const Account *acc) {
    json_t *j = json_object();
    json_object_set_new(j, "name", json_string(acc->name));
    json_object_set_new(j, "email", json_string(acc->email));
    json_object_set_new(j, "password", json_string(acc->password));
    json_object_set_new(j, "time_registered", json_integer(acc->time_registered));
    json_object_set_new(j, "verified", json_integer(acc->verified));

    // Channels array
    json_t *jchannels = json_array();
    if (acc->channels) {
        for (char **c = acc->channels; *c; ++c)
            json_array_append_new(jchannels, json_string(*c));
    }
    json_object_set_new(j, "channels", jchannels);

    // Metadata array
    json_t *jmeta = json_array();
    for (Metadata *m = acc->metadata_head; m; m = m->next) {
        json_t *mj = json_object();
        json_object_set_new(mj, "key", json_string(m->key));
        json_object_set_new(mj, "value", json_string(m->value));
        json_array_append_new(jmeta, mj);
    }
    json_object_set_new(j, "metadata", jmeta);

    return j;
}

/**
 * write_account_to_db - Appends an Account to the account database file.
 * Returns 1 on success, 0 on failure.
 */
int write_account_to_db(const Account *acc) {
    FILE *f = fopen(ACCOUNT_DB_PATH, "a");
    if (!f) return 0;
    json_t *j = account_to_json(acc);
    char *dump = json_dumps(j, JSON_COMPACT);
    fprintf(f, "%s\n", dump);
    free(dump);
    json_decref(j);
    fclose(f);
    return 1;
}

/**
 * read_accounts_from_db - Reads all accounts from the database file.
 * Returns a NULL-terminated array of Account*.
 */
Account **read_accounts_from_db(void) {
    FILE *f = fopen(ACCOUNT_DB_PATH, "r");
    if (!f) return NULL;
    Account **accounts = NULL;
    size_t count = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        json_error_t err;
        json_t *j = json_loads(line, 0, &err);
        if (!j) continue;
        Account *acc = safe_alloc(sizeof(Account));
        acc->name = strdup(json_string_value(json_object_get(j, "name")));
        acc->email = strdup(json_string_value(json_object_get(j, "email")));
        acc->password = strdup(json_string_value(json_object_get(j, "password")));
        acc->time_registered = (time_t)json_integer_value(json_object_get(j, "time_registered"));
        acc->verified = json_integer_value(json_object_get(j, "verified"));

        // Channels
        json_t *jchannels = json_object_get(j, "channels");
        if (json_is_array(jchannels)) {
            size_t n = json_array_size(jchannels);
            acc->channels = safe_alloc((n+1) * sizeof(char*));
            for (size_t i = 0; i < n; ++i)
                acc->channels[i] = strdup(json_string_value(json_array_get(jchannels, i)));
            acc->channels[n] = NULL;
        }

        // Metadata
        json_t *jmeta = json_object_get(j, "metadata");
        if (json_is_array(jmeta)) {
            for (size_t i = 0; i < json_array_size(jmeta); ++i) {
                json_t *mj = json_array_get(jmeta, i);
                add_metadata(acc,
                    json_string_value(json_object_get(mj, "key")),
                    json_string_value(json_object_get(mj, "value")));
            }
        }
        // Add to array
        accounts = realloc(accounts, sizeof(Account*) * (count+2));
        accounts[count++] = acc;
        accounts[count] = NULL;
        json_decref(j);
    }
    fclose(f);
    return accounts;
}

/**
 * MOD_INIT - Module initialization routine.
 * Registers the account registration capability and command.
 */
MOD_INIT()
{
    MARK_AS_GLOBAL_MODULE(modinfo);

    ClientCapabilityInfo accreg_cap; 
	memset(&accreg_cap, 0, sizeof(accreg_cap));
	accreg_cap.name = REGCAP_NAME;
	accreg_cap.visible = accreg_capability_visible;
	accreg_cap.parameter = accreg_capability_parameter;
	if (!ClientCapabilityAdd(modinfo->handle, &accreg_cap, &CAP_ACCOUNTREGISTRATION))
	{
		config_error("Could not add CAP for draft/account-registration. Please contact ObsidianIRC Support.");
		return MOD_FAILED;
	}

    CommandAdd(modinfo->handle, CMD_REGISTER, register_account, 3, CMD_USER|CMD_UNREGISTERED);
    return MOD_SUCCESS;
}

/**
 * MOD_LOAD - Called when the module is loaded.
 */
MOD_LOAD()
{
    return MOD_SUCCESS;
}

/**
 * MOD_UNLOAD - Called when the module is unloaded.
 */
MOD_UNLOAD()
{
    return MOD_SUCCESS;
}

/**
 * MOD_TEST - Called for module self-tests.
 */
MOD_TEST()
{
   return MOD_SUCCESS;
}

/**
 * register_account - Handles the REGISTER command from users.
 * Validates input, checks for bans, hashes password, and stores account.
 */
CMD_FUNC(register_account)
{
    if (IsLoggedIn(client))
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER ALREADY_AUTHENTICATED %s :You are already authenticated to an account.", me.name, client->user->account);
        return;
    }

    if (BadPtr(parv[1]) || BadPtr(parv[2]) || BadPtr(parv[3]))
    {
        sendto_one(client, NULL, ":%s NOTE REGISTER INVALID_PARAMS :Syntax: /REGISTER <account name> <email> <password>", me.name);
        return;
    }

    const char *accname = parv[1];
    const char *email = parv[2];
    const char *password = parv[3];

    if (strlen(accname) < 4)
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :Your account name must be at least 4 characters long.", me.name, accname);
        return;
    }

    Client *found_user = find_client(accname, NULL);
    if (found_user && found_user != client)
    {
        if (client->name) // Don't send before they have NICK first
            sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :That account name is currently in use.", me.name, accname);
        else // Send something generic instead, otherwise it could be interpreted as leaking accounts
            sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :That account name is banned.", me.name, accname);
        return;
    }

    TKL *ban = my_find_tkl_nameban(accname);
    if (ban)
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :That account name is banned.", me.name, accname);
        return;
    }

    // Avoid using server service suffixes like serv, etc.
    const char *end = accname + strlen(accname) - 4;
    if (strcasecmp(end, "serv") == 0)
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :Account names cannot end with 'serv'.", me.name, accname);
        return;
    }

    // Check for existing registration
    Account **accounts = read_accounts_from_db();
    for (int i = 0; accounts && accounts[i]; i++)
    {
        if (strcasecmp(accounts[i]->name, accname) == 0)
        {
            if (client->name) // Don't send before they have NICK first
                sendto_one(client, NULL, ":%s FAIL REGISTER ACCOUNT_EXISTS %s :That account name is already registered.", me.name, accname);
            else // Send something generic instead, otherwise it could be interpreted as leaking accounts
                sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :That account name is banned.", me.name, accname);

            for (int j = 0; accounts[j]; j++)
                free_account(accounts[j]);
            free(accounts);
            return;
        }
    }
    if (accounts)
    {
        for (int j = 0; accounts[j]; j++)
            free_account(accounts[j]);
        free(accounts);
    }
    const char *password_hash = NULL;
	
	if (!(password_hash = Auth_Hash(6, parv[3])))
	{
		sendto_one(client, NULL, ":%s FAIL REGISTER SERVER_BUG %s :The hashing mechanism was not supported. Please contact an administrator.", me.name, parv[1]);
		return;
	}

    // Create and populate account
    Account *acc = safe_alloc(sizeof(Account));
    acc->name = strdup(accname);
    acc->email = strdup(email);
    acc->password = strdup(password_hash);  // NOTE: hash this in production
    acc->time_registered = TStime();
    acc->verified = 0;
    acc->channels = NULL;
    acc->metadata_head = NULL;

    if (!write_account_to_db(acc))
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER INTERNAL_ERROR :Failed to write account to database.", me.name);
        free_account(acc);
        return;
    }

    sendto_one(client, NULL, ":%s REGISTER SUCCESS %s :Account registered successfully.", me.name, accname);

    // Trigger hook for other modules
    RunHook(HOOKTYPE_ACCOUNT_REGISTER, client, acc);

    strlcpy(client->user->account, accname, sizeof(client->user->account));
    user_account_login(NULL, client);

    free_account(acc);
}


/**
 * accreg_capability_parameter - Returns the parameter string for the registration capability.
 */
const char *accreg_capability_parameter(Client *client)
{
	return "before-connect,custom-account-name,email-required";
}

/**
 * accreg_capability_visible - Determines if the registration capability is visible to the client.
 */
int accreg_capability_visible(Client *client)
{
	return 1;
}