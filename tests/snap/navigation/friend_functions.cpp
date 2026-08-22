/// - verify: server

struct Vault {
    int value;

    friend int §(friend_decl)inspect(const Vault& vault);

    friend int §(hidden_def)reveal(const Vault& vault) {
        return vault.value;
    }
};

int §(friend_def)inspect(const Vault& vault) {
    return vault.value;
}

int inspect_vault(const Vault& vault) {
    return §(friend_use)inspect(vault) + §(hidden_use)reveal(vault);
}
