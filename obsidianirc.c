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
        post-install-text
        {
                "The module is installed. Now all you need to do is add a loadmodule line:";
                "loadmodule \"third/obsidian-register\";";
                "The module needs no other configuration.";
                "Once you're good to go, you can finally type in your shell: ./unrealircd rehash";
        }
}
*** <<<MODULE MANAGER END>>>
*/

/* One include for all */
#include "obsidian.h"

ModDataInfo *sasl_md;
long CAP_ACCOUNTREGISTRATION = 0L;
static struct AccountRegistrationConfStruct MyConf;

ModuleHeader MOD_HEADER
= 
{
    "third/obsidianirc", /* Name of module */
    "1.0", /* Version */
    "ObsidianIRC", /* Short description of module */
    "ObsidianIRC Team", /* Author */
    "unrealircd-6", /* Version of UnrealIRCd */
};

/**
 * MOD_INIT - Module initialization routine.
 */
MOD_INIT()
{
    set_accreg_conf(); // Set defaults
    ModDataInfo mreq;

    memset(&mreq, 0, sizeof(mreq));
    mreq.name = "sasl_auth_type";
    mreq.free = sat_free;
    mreq.serialize = sat_serialize;
    mreq.unserialize = sat_unserialize;
    mreq.type = MODDATATYPE_CLIENT;
    if (!(sasl_md = ModDataAdd(modinfo->handle, mreq)))
    {
        config_error("Could not add ModData for sasl_auth_type. Please open an Issue on GitHub: https://github.com/ObsidianIRC/UnrealIRCd-Modules/issues/.");
        return MOD_FAILED;
    }

    if (open_database(OBSIDIAN_DB) != SQLITE_OK)
    {
        config_error("Could not open database. Please open an Issue on GitHub: https://github.com/ObsidianIRC/UnrealIRCd-Modules/issues/.");
        return MOD_FAILED;
    }

    ClientCapabilityInfo accreg_cap; 
    memset(&accreg_cap, 0, sizeof(accreg_cap));
    accreg_cap.name = REGCAP_NAME;
    accreg_cap.visible = accreg_capability_visible;
    accreg_cap.parameter = accreg_capability_parameter;
    if (!ClientCapabilityAdd(modinfo->handle, &accreg_cap, &CAP_ACCOUNTREGISTRATION))
    {
        config_error("Could not add CAP for draft/account-registration. Please open an Issue on GitHub: https://github.com/ObsidianIRC/UnrealIRCd-Modules/issues/.");
        return MOD_FAILED;
    }

    HookAddConstString(modinfo->handle, HOOKTYPE_SASL_MECHS, 0, saslmechs);
    HookAdd(modinfo->handle, HOOKTYPE_SASL_AUTHENTICATE, 0, authenticate_attempt);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, accreg_configrun); // Run through the config and set the values
	
    CommandAdd(modinfo->handle, CMD_REGISTER, register_account, 3, CMD_USER|CMD_UNREGISTERED);
    CommandAdd(modinfo->handle, CMD_LISTACC, list_accounts, 3, CMD_OPER);
    CommandAdd(modinfo->handle, CMD_IDENTIFY, cmd_identify, 2, CMD_USER);
    CommandAdd(modinfo->handle, CMD_LOGOUT, cmd_logout, 0, CMD_USER);
    

    RPCHandlerInfo r;
    memset(&r, 0, sizeof(r));
    r.method = "obsidianirc.accounts.list";
	r.loglevel = ULOG_DEBUG;
	r.call = rpc_list_accounts;
    RPCHandlerAdd(modinfo->handle, &r);

    memset(&r, 0, sizeof(r));
    r.method = "obsidianirc.accounts.find";
	r.loglevel = ULOG_DEBUG;
	r.call = rpc_accounts_find;
    RPCHandlerAdd(modinfo->handle, &r);
    return MOD_SUCCESS;
}

MOD_LOAD()
{
	ModuleSetOptions(modinfo->handle, MOD_OPT_PERM_RELOADABLE, 1);
    if (open_database(OBSIDIAN_DB) != SQLITE_OK)
    {
        config_error("Could not open database. Please contact ObsidianIRC Support.");
        return MOD_FAILED;
    }
    safe_strdup(iConf.sasl_server, me.name);
    moddata_client_set(&me, "saslmechlist", "PLAIN,EXTERNAL");
    return MOD_SUCCESS;
}

/**
 * MOD_UNLOAD - Called when the module is unloaded.
 */
MOD_UNLOAD()
{
    close_database();
    safe_free(iConf.sasl_server);
    iConf.sasl_server = NULL;
    free_accreg_conf();
    return MOD_SUCCESS;
}

/**
 * register_account - Handles the REGISTER command from users.
 * Syntax: REGISTER <name> <email> <password>
 */
CMD_FUNC(register_account)
{
    if (!db && open_database(OBSIDIAN_DB) != SQLITE_OK)
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER SERVER_BUG :Database unavailable.", me.name);
        return;
    }
    if (parc < 4)
    {
        sendto_one(client, NULL, ":%s NOTE REGISTER INVALID_PARAMS :Syntax: /REGISTER <name> <email> <password>", me.name);
        return;
    }

    const char *name = parv[1];
    const char *email = parv[2];
    const char *password = parv[3];

    if (strlen(name) < MyConf.min_name_length || strlen(name) > MyConf.max_name_length)
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :Your account name must be between %d and %d characters long.", me.name, name, MyConf.min_name_length, MyConf.max_name_length);
        return;
    }

    if (strlen(password) < MyConf.min_password_length || strlen(password) > MyConf.max_password_length)
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER BAD_PASSWORD %s :Your password must be between %d and %d characters long.", me.name, name, MyConf.min_password_length, MyConf.max_password_length);
        return;
    }

    if (MyConf.require_email
        && (strlen(email) < 5
        || !strcmp(email, "*")
        || (!strchr(email, '@')
        || !strchr(email, '.')))
    )
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER BAD_EMAIL %s :You must provide a valid email address.", me.name, name);
        return;
    }

    Client *found_user = find_client(name, NULL);
    if (found_user && found_user != client)
    {
        if (client->name) // Don't send before they have NICK first
        {
            sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :That account name is currently in use.", me.name, name);
        }
        else // Send something generic instead, otherwise it could be interpreted as leaking accounts
        {
            sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :That account name is banned.", me.name, name);
        }
        return;
    }

    TKL *ban = my_find_tkl_nameban(name);
    if (ban)
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER BAD_ACCOUNT_NAME %s :That account name is banned.", me.name, name);
        return;
    }

    // Check if account already exists
    Account *existing = find_account(name);
    if (existing)
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER ACCOUNT_EXISTS %s :That account name is already registered.", me.name, name);
        free_account(existing);
        return;
    }
    const char *password_hash = NULL;
    
    if (!(password_hash = Auth_Hash(6, parv[3])))
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER SERVER_BUG %s :The hashing mechanism was not supported. Please contact an administrator.", me.name, parv[1]);
        return;
    }
    // Create and store new account
    Account *acc = safe_alloc(sizeof(Account));
    acc->name = strdup(name);
    acc->email = strdup(email);
    acc->password = strdup(password_hash);
    acc->time_registered = time(NULL);
    acc->verified = 0;
    acc->channels = NULL;
    acc->metadata_head = NULL;

    if (write_account_to_db(acc))
    {
        sendto_one(client, NULL, ":%s REGISTER SUCCESS %s :Account registered successfully.", me.name, name);
        strlcpy(client->user->account, name, sizeof(client->user->account));
        user_account_login(NULL, client);
        unreal_log(ULOG_INFO, "account", "REGISTER", client,
            "New account registered by $client.details [account: $account] [email: $email]", 
            log_data_string("account", acc->name),
            log_data_string("email", acc->email)
        );
        RunHook(HOOKTYPE_ACCOUNT_REGISTER, acc, client);
    }
    else
    {
        sendto_one(client, NULL, ":%s FAIL REGISTER INTERNAL_ERROR :Failed to register account.", me.name);
    }
    free_account(acc);
}

/**
 * list_accounts - Handles the LISTACC command from users.
 */
CMD_FUNC(list_accounts)
{
    Account **accounts = read_accounts_from_db(!BadPtr(parv[1]) ? parv[1] : NULL);
    if (!accounts || !accounts[0])
    {
        sendto_one(client, NULL, ":%s LISTACC NO_ACCOUNTS :No accounts registered.", me.name);
        if (accounts)
        {
            free(accounts);
        }
        return;
    }
    for (size_t i = 0; accounts[i]; i++)
    {
        // Count logged-in users (members)
        int member_count = 0;
        AccountMember *m = accounts[i]->members;
        while (m)
        {
            member_count++;
            m = m->next;
        }
        sendto_one(client, NULL, ":%s LISTACC ACCOUNT %ld %s %s %ld %d %d",
            me.name,
            accounts[i]->id,
            accounts[i]->name,
            accounts[i]->email,
            (long)accounts[i]->time_registered,
            accounts[i]->verified,
            member_count // number of logged-in users
        );
        free_account(accounts[i]);
    }
    free(accounts);
}

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
        {
            continue;
        }
        if (!strcasecmp(name, tkl->ptr.nameban->name))
        {
            return tkl;
        }
    }
    return NULL;
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

/**
 * open_database - Opens the SQLite database.
 */
int open_database(const char *filename)
{
    if (sqlite3_open(filename, &db) != SQLITE_OK)
    {
        return SQLITE_ERROR;
    }
    const char *sql =   "CREATE TABLE IF NOT EXISTS accounts ("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                        "name TEXT, "
                        "email TEXT, "
                        "password TEXT, "
                        "time_registered INTEGER, "
                        "verified INTEGER)";
    char *errmsg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK)
    {
        sqlite3_free(errmsg);
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

/**
 * close_database - Closes the SQLite database.
 */
void close_database()
{
    if (db)
    {
        sqlite3_close(db);
        db = NULL;
    }
}

/**
 * free_account - Frees all memory associated with an Account struct.
 */
void free_account(Account *acc)
{
    if (!acc)
    {
        return;
    }
    free(acc->name);
    free(acc->email);
    free(acc->password);
    if (acc->channels)
    {
        for (char **c = acc->channels; *c; c++)
        {
            free(*c);
        }
        free(acc->channels);
    }
    free_metadata(acc->metadata_head);
    free(acc);
}

/**
 * write_account_to_db - Appends an Account to the account database.
 * Returns 1 on success, 0 on failure.
 */
int write_account_to_db(const Account *acc)
{
    if (find_account(acc->name))
    {
        // Account already exists
        return 0;
    }
    const char *sql = "INSERT INTO accounts (name, email, password, time_registered, verified) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        return 0;
    }

    sqlite3_bind_text(stmt, 1, acc->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, acc->email, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, acc->password, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, (int)acc->time_registered);
    sqlite3_bind_int(stmt, 5, acc->verified);

    int result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return result == SQLITE_DONE ? 1 : 0;
}

/**
 * read_accounts_from_db - Reads all accounts from the database.
 * Returns a NULL-terminated array of Account*.
 */
Account **read_accounts_from_db(const char *name)
{
    const char *sql_all = "SELECT * FROM accounts";
    const char *sql_one = "SELECT * FROM accounts WHERE lower(name) = lower(?) LIMIT 1";
    sqlite3_stmt *stmt;
    Account **accounts = NULL;
    size_t count = 0;

    if (!db)
    {
        return NULL;
    }

    if (sqlite3_prepare_v2(db, name ? sql_one : sql_all, -1, &stmt, NULL) != SQLITE_OK)
    {
        return NULL;
    }

    if (name)
    {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Account *acc = safe_alloc(sizeof(Account));
        acc->id = sqlite3_column_int(stmt, 0);
        acc->name = strdup((const char *)sqlite3_column_text(stmt, 1));
        acc->email = strdup((const char *)sqlite3_column_text(stmt, 2));
        acc->password = strdup((const char *)sqlite3_column_text(stmt, 3));
        acc->time_registered = (time_t)sqlite3_column_int(stmt, 4);
        acc->verified = sqlite3_column_int(stmt, 5);
        acc->channels = NULL;
        acc->metadata_head = NULL;
        acc->members = NULL;

        // Populate online clients for this account
        Client *cptr;
        list_for_each_entry(cptr, &client_list, client_node)
        {
            if (cptr->user && cptr->user->account && strcmp(cptr->user->account, acc->name) == 0)
            {
                AccountMember *member = safe_alloc(sizeof(AccountMember));
                member->client = cptr;
                member->next = acc->members;
                acc->members = member;
            }
        }

        Account **tmp = realloc(accounts, sizeof(Account*) * (count + 2));
        if (!tmp)
        {
            for (size_t i = 0; accounts && accounts[i]; i++)
            {
                free_account(accounts[i]);
            }
            free(accounts);
            sqlite3_finalize(stmt);
            return NULL;
        }
        accounts = tmp;
        accounts[count++] = acc;
    }

    sqlite3_finalize(stmt);

    if (!accounts)
    {
        accounts = safe_alloc(sizeof(Account*));
        accounts[0] = NULL;
    }
    else
    {
        accounts[count] = NULL;
    }

    return accounts;
}

Account *find_account_by_client(Client *client)
{
    if (!db || !client || !client->name)
    {
        return NULL;
    }

    return find_account(client->user->account);
}

Account *find_account(const char *name)
{
    if (!db || !name)
    {
        return NULL;
    }
    Account **accounts = read_accounts_from_db(name);
    if (!accounts)
    {
        return NULL;
    }
    Account *result = NULL;
    if (accounts[0])
    {
        result = accounts[0];
        accounts[0] = NULL;
    }
    // Free the rest (should be none, but for safety)
    for (size_t i = 0; accounts[i]; i++)
    {
        if (accounts[i])
        {
            free_account(accounts[i]);
        }
    }
    free(accounts);
    return result;
}

// Create a new metadata node
Metadata* create_metadata(const char *key, const char *value)
{
    Metadata *m = safe_alloc(sizeof(Metadata));
    m->key = strdup(key);
    m->value = strdup(value);
    m->prev = m->next = NULL;
    return m;
}

/**
 * add_metadata - Adds a metadata key/value pair to an Account.
 */
void add_metadata(Account *acc, const char *key, const char *value)
{
    Metadata *m = create_metadata(key, value);
    m->next = acc->metadata_head;
    if (acc->metadata_head)
    {
        acc->metadata_head->prev = m;
    }
    acc->metadata_head = m;
}

/**
 * free_metadata - Frees a linked list of Metadata nodes.
 */
void free_metadata(Metadata *head)
{
    Metadata *cur = head, *tmp;
    while (cur)
    {
        tmp = cur->next;
        free(cur->key);
        free(cur->value);
        free(cur);
        cur = tmp;
    }
}

/**
 * authenticate_attempt - Hook to handle authentication attempts.
 * This is called when a client attempts to authenticate when we
 * are the SASL server.
 */
int authenticate_attempt(Client *client, int first, const char *param)
{
    if (!SASL_SERVER || !MyConnect(client) || !param || !*param)
    {
        return 0;
    }
    if (!strcmp(param, "*")) // abort
    {
        if (GetSaslType(client))
        {
            DelSaslType(client);
            return 0;
        }
    }
    else if (!strcmp(param,"PLAIN"))
    {
        SetSaslType(client, SASL_TYPE_PLAIN);
        sendto_one(client, NULL, ":%s AUTHENTICATE +", me.name);
        return 0;
    }
    else if (!strcmp(param, "ANONYMOUS"))
    {
        strlcpy(client->user->account, "0", sizeof("0"));
        user_account_login(NULL, client);

        if (IsDead(client)) // Can be dead at this point
        {
            return 0;
        }

        if (GetSaslType(client))
        {
            client->local->sasl_complete = 0;
            sendnumeric(client, RPL_SASLSUCCESS);
            DelSaslType(client);
        }

        return 0;
    }
    else if (!strcmp(param, "EXTERNAL"))
    {
        SetSaslType(client, SASL_TYPE_EXTERNAL);
    }

    if (!GetSaslType(client) || GetSaslType(client) == SASL_TYPE_NONE)
    {
        return 0;
    }
    else if (GetSaslType(client) == SASL_TYPE_PLAIN)
    {
        char *auth, *username, *password;

        if (!decode_authenticate_plain(param, &auth, &username, &password))
        {
            sendnumeric(client, ERR_SASLFAIL);
            return 0;
        }

        if (BadPtr(username) || BadPtr(password))
        {
            sendnumeric(client, ERR_SASLFAIL);
            return 0;
        }

        Account *account = find_account(username);
        if (account && argon2_verify(account->password, password, strlen(password), Argon2_id) == ARGON2_OK)
        {
            strlcpy(client->user->account, account->name, sizeof(client->user->account));
            unreal_log(ULOG_INFO, "account", "LOGIN", client,
                "User $client.details logged in [account: $account] [email: $email]",
                log_data_string("email", account->email),
                log_data_string("account", account->name)
            );
            user_account_login(NULL, client);
            sendnumeric(client, RPL_SASLSUCCESS);
            client->local->sasl_complete = 1;
            DelSaslType(client);
        }
        else
        {
            client->local->sasl_sent_time = 0;
            add_fake_lag(client, 7000);
            sendnumeric(client, ERR_SASLFAIL);
        }
        free_account(account);
        return 0;
    }
    else if (GetSaslType(client) == SASL_TYPE_EXTERNAL)
    {
        // TODO: Handle EXTERNAL authentication
    }

    return 0;
}

const char *saslmechs(Client *client)
{
    return "PLAIN,ANONYMOUS";
}

const char *sat_serialize(ModData *m)
{
    static char buf[32];
    if (m->i == 0)
    {
        return NULL; /* not set */
    }
    snprintf(buf, sizeof(buf), "%d", m->i);
    return buf;
}

void sat_free(ModData *m)
{
    m->i = 0;
}

void sat_unserialize(const char *str, ModData *m)
{
    m->i = atoi(str);
}


/**
 * account2json - Converts an Account struct to a JSON object.
 * Returns a new json_t* object.
 */
json_t* account2json(const Account *acc) {
    json_t *j = json_object();
    json_object_set_new(j, "id", acc->id ? json_integer(acc->id) : 0);
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
    for (Metadata *m = acc->metadata_head; m; m = m->next)
    {
        json_t *mj = json_object();
        json_object_set_new(mj, "key", json_string(m->key));
        json_object_set_new(mj, "value", json_string(m->value));
        json_array_append_new(jmeta, mj);
    }
    json_object_set_new(j, "metadata", jmeta);
    
    json_t *jmembers = json_object();
    for (AccountMember *m = acc->members; m; m = m->next)
        json_expand_client(jmembers, m->client->id, m->client, 2);
    
    json_object_set_new(j, "online_clients", jmembers);
    
    return j;
}

RPC_CALL_FUNC(rpc_list_accounts)
{
    if (!db)
    {
        rpc_error(client, request, JSON_RPC_ERROR_INTERNAL_ERROR, "Database is not available.");
        return;
    }

    Account **accounts = read_accounts_from_db(NULL);
    if (!accounts || !accounts[0])
    {
        free(accounts);
        rpc_error(client, request, JSON_RPC_ERROR_NOT_FOUND, "No accounts registered.");
        return;
    }

    json_t *jaccounts = json_array(), *result = json_object();
    for (size_t i = 0; accounts[i]; i++)
    {
        json_t *jacc = account2json(accounts[i]);
        json_array_append_new(jaccounts, jacc);
        free_account(accounts[i]);
    }
    free(accounts);
    json_object_set_new(result, "accounts", jaccounts);
    rpc_response(client, request, result);
    json_decref(result);
}

RPC_CALL_FUNC(rpc_accounts_find)
{
    if (!db)
    {
        rpc_error(client, request, JSON_RPC_ERROR_INTERNAL_ERROR, "Database is not available.");
        return;
    }
    const char *name;
    REQUIRE_PARAM_STRING("name", name);

    Account *acc = find_account(name);
    if (!acc)
    {
        rpc_error(client, request, JSON_RPC_ERROR_NOT_FOUND, "Account not found.");
        return;
    }

    json_t *jacc = account2json(acc);
    rpc_response(client, request, jacc);
    json_decref(jacc);
    free_account(acc);
}

// For users who don't support SASL, we provide a way to identify to an account
// using the IDENTIFY command. This is a fallback for those who cannot use SASL.
CMD_FUNC(cmd_identify)
{
    if (!db && open_database(OBSIDIAN_DB) != SQLITE_OK)
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY SERVER_BUG :Database unavailable.", me.name);
        return;
    }
    if (parc < 3)
    {
        sendto_one(client, NULL, ":%s NOTE IDENTIFY INVALID_PARAMS :Syntax: /IDENTIFY <account> <password>", me.name);
        return;
    }
    const char *account_name = parv[1];
    if (!account_name || !*account_name)
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY INVALID_ACCOUNT :Account name cannot be empty.", me.name);
        return;
    }
    if (!strcasecmp(account_name, client->user->account))
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY ALREADY_IDENTIFIED :You are already identified to account %s.", me.name, account_name);
        return;
    }
    if (strlen(account_name) < 4)
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY INVALID_ACCOUNT :Account name must be at least 4 characters long.", me.name);
        return;
    }
    Client *found_user = find_client(account_name, NULL);
    if (found_user && found_user != client)
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY INVALID_ACCOUNT :That account name is currently in use.", me.name);
        return;
    }
    TKL *ban = my_find_tkl_nameban(account_name);
    if (ban)
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY INVALID_ACCOUNT :That account name is banned.", me.name);
        return;
    }

    const char *password = parv[2];
    if (!password || !*password)
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY INVALID_PASSWORD :Password cannot be empty.", me.name);
        return;
    }

    if (!client->user || !client->user->account || !*client->user->account)
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY NOT_LOGGED_IN :You must be logged in to identify.", me.name);
        return;
    }
    Account *acc = find_account(account_name);
    if (!acc)
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY ACCOUNT_NOT_FOUND :Account %s not found.", me.name, account_name);
        return;
    }

    if (argon2_verify(acc->password, password, strlen(password), Argon2_id) == ARGON2_OK)
    {
        sendto_one(client, NULL, ":%s IDENTIFY SUCCESS %s :You have been successfully identified.", me.name, acc->name);
        strlcpy(client->user->account, acc->name, sizeof(client->user->account));
        user_account_login(NULL, client);
        DelSaslType(client);
        unreal_log(ULOG_INFO, "account", "IDENTIFY", client,
            "User $client.details identified [account: $account] [email: $email]",
            log_data_string("email", acc->email),
            log_data_string("account", acc->name)
        );
    }
    else
    {
        sendto_one(client, NULL, ":%s FAIL IDENTIFY INVALID_PASSWORD :Invalid password for account %s.", me.name, acc->name);
        client->local->sasl_sent_time = 0;
        add_fake_lag(client, 7000);
    }
    
    free_account(acc);
}

CMD_FUNC(cmd_logout)
{
    if (!IsLoggedIn(client))
    {
        sendto_one(client, NULL, ":%s FAIL LOGOUT NOT_LOGGED_IN :You are not logged in.", me.name);
        return;
    }
    strlcpy(client->user->account, "0", sizeof(client->user->account));
    user_account_login(NULL, client);
    sendto_one(client, NULL, ":%s LOGOUT SUCCESS :You have been logged out successfully.", me.name);
}



// Set defaults for the configuration settings here (called in MOD_INIT)
void set_accreg_conf(void)
{
    MyConf.min_name_length = 3;
    MyConf.max_name_length = 50;
    MyConf.min_password_length = 8;
    MyConf.max_password_length = 200;
    MyConf.require_email = 1;
    MyConf.require_terms_acceptance = 1;
    MyConf.allow_username_changes = 1;
    MyConf.allow_password_changes = 1;
    MyConf.allow_email_changes = 1;
    safe_strdup(MyConf.guest_nick_format, "Guest$d$d$d$d")
}

// Free the memory allocated for the configuration settings here (called in MOD_UNLOAD)
void free_accreg_conf(void)
{
    safe_free(MyConf.guest_nick_format);
}

// Configuration testing function (check for errors in the config block) (called in MOD_TEST)
int accreg_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
    int errors = 0;
    ConfigEntry *cep;

    if(type != CONFIG_MAIN)
        return 0;
    if(!ce || !ce->name)
        return 0;
    if(strcmp(ce->name, CONF_ACCOUNT_BLOCK))
        return 0;

    for(cep = ce->items; cep; cep = cep->next)
    {
        if(!cep->value)
        {
            config_error("%s:%i: blank %s value", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK);
            errors++;
            continue;
        }
		if(!cep->name)
		{
			config_error("%s:%i: blank %s item name", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK);
			errors++;
			continue;
		}
		if (!strcmp(cep->name, "min-name-length"))
		{
			if (MyConf.got_min_name_length)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.min_name_length = atoi(cep->value);
			if (MyConf.min_name_length < MIN_ACCOUNT_NAME_LENGTH || MyConf.min_name_length > MAX_ACCOUNT_NAME_LENGTH)
			{
				config_error("%s:%i: %s::%s must be between %d and %d", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name, MIN_ACCOUNT_NAME_LENGTH, MAX_ACCOUNT_NAME_LENGTH);
				errors++;
			}
			MyConf.got_min_name_length = true;
			continue;
		}
		if (!strcmp(cep->name, "max-name-length"))
		{
			if (MyConf.got_max_name_length)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.max_name_length = atoi(cep->value);
			if (MyConf.max_name_length < MIN_ACCOUNT_NAME_LENGTH || MyConf.max_name_length > MAX_ACCOUNT_NAME_LENGTH)
			{
				config_error("%s:%i: %s::%s must be between %d and %d", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name, MIN_ACCOUNT_NAME_LENGTH, MAX_ACCOUNT_NAME_LENGTH);
				errors++;
			}
			MyConf.got_max_name_length = true;
			continue;
		}
		if (!strcmp(cep->name, "min-password-length"))
		{
			if (MyConf.got_min_password_length)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.min_password_length = atoi(cep->value);
			if (MyConf.min_password_length < MIN_PASSWORD_LENGTH || MyConf.min_password_length > MAX_PASSWORD_LENGTH)
			{
				config_error("%s:%i: %s::%s must be between %d and %d", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name, MIN_PASSWORD_LENGTH, MAX_PASSWORD_LENGTH);
				errors++;
			}
			MyConf.got_min_password_length = true;
			continue;
		}
		if (!strcmp(cep->name, "max-password-length"))
		{
			if (MyConf.got_max_password_length)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.max_password_length = atoi(cep->value);
			if (MyConf.max_password_length < MIN_PASSWORD_LENGTH || MyConf.max_password_length > MAX_PASSWORD_LENGTH)
			{
				config_error("%s:%i: %s::%s must be between %d and %d", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name, MIN_PASSWORD_LENGTH, MAX_PASSWORD_LENGTH);
				errors++;
			}
			MyConf.got_max_password_length = true;
			continue;
		}
		if (!strcmp(cep->name, "require-email"))
		{
			if (MyConf.got_require_email)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.got_require_email = true;
			continue;
		}
		if (!strcmp(cep->name, "require-terms-acceptance"))
		{
			if (MyConf.got_require_terms_acceptance)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.got_require_terms_acceptance = true;
			continue;
		}
		if (!strcmp(cep->name, "allow-username-changes"))
		{
			if (MyConf.got_allow_username_changes)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.got_allow_username_changes = true;
			continue;
		}
		if (!strcmp(cep->name, "allow-password-changes"))
		{
			if (MyConf.got_allow_password_changes)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.got_allow_password_changes = true;
			continue;
		}
		if (!strcmp(cep->name, "allow-email-changes"))
		{
			if (MyConf.got_allow_email_changes)
			{
				config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
				errors++;
			}
			MyConf.got_allow_email_changes = true;
			continue;
		}
        if (!strcmp(cep->name, "guest-nick-format"))
        {
            if (MyConf.got_guest_nick_format)
            {
                config_error("%s:%i: duplicate %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
                errors++;
            }
            if (BadPtr(cep->value))
            {
                config_error("%s:%i: %s::%s cannot be empty", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
                errors++;
            }
            MyConf.got_guest_nick_format = true;
            continue;
        }
        // Unknown directive, warn about it
        config_warn("%s:%i: unknown item %s::%s", cep->file->filename, cep->line_number, CONF_ACCOUNT_BLOCK, cep->name);
    }
    *errs = errors;
    return errors ? -1 : 1;
}


// Run through the configuration and set the values (called in MOD_INIT, after MOD_TEST)
int accreg_configrun(ConfigFile *cf, ConfigEntry *ce, int type)
{
    ConfigEntry *cep;

    if(type != CONFIG_MAIN)
        return 0;
    if(!ce || !ce->name)
        return 0;
    if(strcmp(ce->name, CONF_ACCOUNT_BLOCK))
        return 0;

    for(cep = ce->items; cep; cep = cep->next)
    {
        if(!cep->name)
            continue;

        if(!strcmp(cep->name, "min-name-length"))
        {
            MyConf.min_name_length = atoi(cep->value);
            continue;
        }
        if(!strcmp(cep->name, "max-name-length"))
        {
            MyConf.max_name_length = atoi(cep->value);
            continue;
        }
        if(!strcmp(cep->name, "min-password-length"))
        {
            MyConf.min_password_length = atoi(cep->value);
            continue;
        }
        if(!strcmp(cep->name, "max-password-length"))
        {
            MyConf.max_password_length = atoi(cep->value);
            continue;
        }
        if(!strcmp(cep->name, "require-email"))
        {
            MyConf.require_email = config_checkval(cep->value, CFG_YESNO);
            continue;
        }
        if(!strcmp(cep->name, "require-terms-acceptance"))
        {
            MyConf.require_terms_acceptance = config_checkval(cep->value, CFG_YESNO);
            continue;
        }
        if(!strcmp(cep->name, "allow-username-changes"))
        {
            MyConf.allow_username_changes = config_checkval(cep->value, CFG_YESNO);
            continue;
        }
        if(!strcmp(cep->name, "allow-password-changes"))
        {
            MyConf.allow_password_changes = config_checkval(cep->value, CFG_YESNO);
            continue;
        }
        if(!strcmp(cep->name, "allow-email-changes"))
        {
            MyConf.allow_email_changes = config_checkval(cep->value, CFG_YESNO);
            continue;
        }
        if (!strcmp(cep->name, "guest-prefix"))
        {
            if (MyConf.guest_nick_format)
            {
                free(MyConf.guest_nick_format);
            }
            MyConf.guest_nick_format = strdup(cep->value);
            continue;
        }
    }

    return 1;
}

// Convert $d to a random digit and $n to a users nick
char *convert_guest_nick_format(const char *format, Client *client)
{
    if (!format || !*format)
    {
        return NULL;
    }
    size_t len = strlen(format);
    char *result = safe_alloc(len + 1);
    size_t j = 0;

    for (size_t i = 0; i < len; i++)
    {
        if (format[i] == '$')
        {
            i++;
            if (i < len && format[i] == 'd') // Random digit
            {
                result[j++] = '0' + (rand() % 10);
            }
            else if (i < len && format[i] == 'n' && client && client->name) // User's nick
            {
                strlcpy(result + j, client->name, len - j + 1);
                j += strlen(client->name);
            }
            else
            {
                result[j++] = '$'; // Just a literal $
                if (i < len) result[j++] = format[i];
            }
        }
        else
        {
            result[j++] = format[i];
        }
    }
    result[j] = '\0';
    return result;
}
