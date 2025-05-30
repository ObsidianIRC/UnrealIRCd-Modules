#ifndef OBSIDIAN_ACCOUNT_H
#define OBSIDIAN_ACCOUNT_H

// Database files
#define ACCOUNT_DB_PATH "../data/obsidian-account.db"

// Hooks
#define HOOKTYPE_ACCOUNT_REGISTER 150 // Custom hook to tell other modules about account registration

typedef struct Metadata {
    char *key;
    char *value;
    struct Metadata *prev, *next;
} Metadata;

typedef struct Account {
    char *name;
    char *email;
    char *password;
    time_t time_registered;
    int verified;
    char **channels;
    Metadata *metadata_head;
} Account;

// Function declarations
Account **read_accounts_from_db(void);
int write_account_to_db(const Account *acc);
json_t* account_to_json(const Account *acc);
void free_account(Account *acc);
TKL *my_find_tkl_nameban(const char *name);
#endif