#ifndef OBSIDIAN_H
#define OBSIDIAN_H

// Includes
#include "unrealircd.h"
#include "sqlite3.h"

// Database files
#define OBSIDIAN_DB "../data/obsidian.db"

// Commands
#define CMD_REGISTER "REGISTER"
#define CMD_LISTACC "LISTACC"

// Command functions
CMD_FUNC(register_account);
CMD_FUNC(list_accounts);

// Capabilities
#define REGCAP_NAME "draft/account-registration"

// Hooks definitions
#define HOOKTYPE_ACCOUNT_REGISTER 150 // Custom hook to tell other modules about account registration

// Hooks
int authenticate_attempt(Client *client, int first, const char *param);
const char *saslmechs(Client *client);

// SASL definitions
#define SASL_TYPE_NONE 0
#define SASL_TYPE_PLAIN 1
#define SASL_TYPE_EXTERNAL 2
#define SASL_TYPE_ANONYMOUS 3 // logout
#define SASL_TYPE_SESSION_COOKIE 4
#define SASL_TYPE_OTP 5
#define GetSaslType(x)			(moddata_client(x, sasl_md).i)
#define SetSaslType(x, y)		do { moddata_client(x, sasl_md).i = y; } while (0)
#define DelSaslType(x)		do { moddata_client(x, sasl_md).i = SASL_TYPE_NONE; } while (0)

// SASL MD serialization
extern ModDataInfo *sasl_md;
void sat_free(ModData *m);
const char *sat_serialize(ModData *m);
void sat_unserialize(const char *str, ModData *m);

// Structs
// IRCv3 METADATA
typedef struct Metadata {
    bool ircv3; // true if this is an IRCv3 metadata item;
    char *key;
    char *value;
    struct Metadata *prev, *next;
} Metadata;

// List of online users who are logged into this account
typedef struct AccountMember {
    Client *client;
    struct AccountMember *next;
} AccountMember;

typedef struct Account {
    long int id; // Unique ID, can be NULL if not used
    char *name;
    char *email;
    char *password;
    time_t time_registered;
    int verified;
    char **channels;
    Metadata *metadata_head;
    AccountMember *members;
} Account;

// Global variables
sqlite3 *db;

// Function declarations
int open_database(const char *filename);
void close_database();
int write_account_to_db(const Account *acc);
Account **read_accounts_from_db(const char *name);
Account *find_account(const char *name);
json_t* account2json(const Account *acc);
void free_account(Account *acc);
void free_metadata(Metadata *head);
Metadata* create_metadata(const char *key, const char *value);
void add_metadata(Account *acc, const char *key, const char *value);
TKL *my_find_tkl_nameban(const char *name);
const char *accreg_capability_parameter(Client *client);
int accreg_capability_visible(Client *client);

#endif
