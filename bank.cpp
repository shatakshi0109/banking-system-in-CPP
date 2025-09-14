// bank.cpp
// Simple banking system using C++ OOP + MySQL C API
// Compile: g++ bank.cpp -o bank -lmysqlclient -std=c++17
// Run: ./bank
//
// Make sure you have libmysqlclient installed, and update DB credentials below.

#include <iostream>
#include <sstream>
#include <vector>
#include <iomanip>
#include <stdexcept>
#include <mysql/mysql.h>   // MySQL C API
#include <string>
#include <memory>

using std::cout;
using std::cin;
using std::endl;
using std::string;

// ---------- CONFIGURE DB CREDENTIALS HERE ----------
const char* DB_HOST = "localhost";
const char* DB_USER = "bankuser";     // change as needed
const char* DB_PASS = "bankpass";     // change as needed
const char* DB_NAME = "bank_system";
const unsigned int DB_PORT = 3306;
// ---------------------------------------------------

// RAII wrapper for MYSQL*
struct MySQLConnection {
    MYSQL *conn;
    MySQLConnection() : conn(mysql_init(nullptr)) {
        if (!conn) throw std::runtime_error("mysql_init() failed");
        // optional: set connection timeout etc.
    }
    ~MySQLConnection() {
        if (conn) mysql_close(conn);
    }
    void connect() {
        if (!mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASS, DB_NAME, DB_PORT, nullptr, 0)) {
            std::ostringstream oss;
            oss << "Connection failed: " << mysql_error(conn);
            throw std::runtime_error(oss.str());
        }
        // set autocommit true by default
        mysql_autocommit(conn, 1);
    }
};

// Utility: escape string for SQL
string sql_escape(MYSQL *conn, const string &s) {
    std::string out;
    out.resize(s.size() * 2 + 1);
    unsigned long newlen = mysql_real_escape_string(conn, &out[0], s.c_str(), (unsigned long)s.size());
    out.resize(newlen);
    return out;
}

// ---------- Domain classes (lightweight) ----------
struct Customer {
    int id;
    string name;
    string email;
    string phone;

    Customer(): id(0) {}
};

struct Account {
    int id;
    int customer_id;
    string account_type;
    double balance;

    Account(): id(0), customer_id(0), balance(0.0) {}
};

struct TransactionRecord {
    int id;
    int account_id;
    string tx_type;
    double amount;
    string timestamp;
    string remarks;
};

// ---------- BankingService (talks to DB) ----------
class BankingService {
private:
    MySQLConnection db;
public:
    BankingService() {
        db.connect();
    }

    // Create a customer and return customer_id
    int create_customer(const string &name, const string &email, const string &phone) {
        string ename = sql_escape(db.conn, name);
        string eemail = sql_escape(db.conn, email);
        string ephone = sql_escape(db.conn, phone);

        std::ostringstream q;
        q << "INSERT INTO customers(name,email,phone) VALUES('"
          << ename << "','" << eemail << "','" << ephone << "')";
        if (mysql_query(db.conn, q.str().c_str())) {
            throw std::runtime_error(mysql_error(db.conn));
        }
        return (int)mysql_insert_id(db.conn);
    }

    // Create an account for a customer
    int create_account(int customer_id, const string &type, double initial_deposit) {
        std::ostringstream q;
        q << "INSERT INTO accounts(customer_id,account_type,balance) VALUES("
          << customer_id << ",'" << sql_escape(db.conn, type) << "'," << std::fixed << std::setprecision(2) << initial_deposit << ")";
        if (mysql_query(db.conn, q.str().c_str())) {
            throw std::runtime_error(mysql_error(db.conn));
        }
        int acc_id = (int)mysql_insert_id(db.conn);
        if (initial_deposit > 0.0) {
            add_transaction(acc_id, "DEPOSIT", initial_deposit, "Initial deposit");
        }
        return acc_id;
    }

    // Helper: get account (simple)
    Account get_account(int account_id) {
        std::ostringstream q;
        q << "SELECT account_id, customer_id, account_type, balance FROM accounts WHERE account_id=" << account_id << " LIMIT 1";
        if (mysql_query(db.conn, q.str().c_str())) {
            throw std::runtime_error(mysql_error(db.conn));
        }
        MYSQL_RES *res = mysql_store_result(db.conn);
        if (!res) throw std::runtime_error(mysql_error(db.conn));
        Account a;
        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row) {
            mysql_free_result(res);
            throw std::runtime_error("Account not found");
        }
        a.id = atoi(row[0]);
        a.customer_id = atoi(row[1]);
        a.account_type = row[2] ? row[2] : "";
        a.balance = row[3] ? atof(row[3]) : 0.0;
        mysql_free_result(res);
        return a;
    }

    // Add transaction record
    void add_transaction(int account_id, const string &tx_type, double amount, const string &remarks) {
        std::ostringstream q;
        q << "INSERT INTO transactions(account_id,tx_type,amount,remarks) VALUES("
          << account_id << ",'" << sql_escape(db.conn, tx_type) << "'," << std::fixed << std::setprecision(2) << amount << ",'" << sql_escape(db.conn, remarks) << "')";
        if (mysql_query(db.conn, q.str().c_str())) {
            throw std::runtime_error(mysql_error(db.conn));
        }
    }

    // Deposit
    void deposit(int account_id, double amount) {
        if (amount <= 0) throw std::runtime_error("Deposit amount must be positive");
        std::ostringstream q;
        q << "UPDATE accounts SET balance = balance + " << std::fixed << std::setprecision(2) << amount << " WHERE account_id=" << account_id;
        if (mysql_query(db.conn, q.str().c_str())) throw std::runtime_error(mysql_error(db.conn));
        add_transaction(account_id, "DEPOSIT", amount, "Deposit");
    }

    // Withdraw
    void withdraw(int account_id, double amount) {
        if (amount <= 0) throw std::runtime_error("Withdraw amount must be positive");
        // check balance
        Account a = get_account(account_id);
        if (a.balance < amount) throw std::runtime_error("Insufficient funds");
        std::ostringstream q;
        q << "UPDATE accounts SET balance = balance - " << std::fixed << std::setprecision(2) << amount << " WHERE account_id=" << account_id;
        if (mysql_query(db.conn, q.str().c_str())) throw std::runtime_error(mysql_error(db.conn));
        add_transaction(account_id, "WITHDRAW", amount, "Withdrawal");
    }

    // Transfer (atomic)
    void transfer(int from_acc, int to_acc, double amount) {
        if (amount <= 0) throw std::runtime_error("Transfer amount must be positive");
        // start transaction
        if (mysql_query(db.conn, "START TRANSACTION")) throw std::runtime_error(mysql_error(db.conn));
        try {
            Account a_from = get_account(from_acc); // will throw if not exist
            Account a_to = get_account(to_acc);

            if (a_from.balance < amount) throw std::runtime_error("Insufficient funds in source account");

            // debit
            std::ostringstream q1;
            q1 << "UPDATE accounts SET balance = balance - " << std::fixed << std::setprecision(2) << amount << " WHERE account_id=" << from_acc;
            if (mysql_query(db.conn, q1.str().c_str())) throw std::runtime_error(mysql_error(db.conn));

            // credit
            std::ostringstream q2;
            q2 << "UPDATE accounts SET balance = balance + " << std::fixed << std::setprecision(2) << amount << " WHERE account_id=" << to_acc;
            if (mysql_query(db.conn, q2.str().c_str())) throw std::runtime_error(mysql_error(db.conn));

            // transactions
            add_transaction(from_acc, "TRANSFER_OUT", amount, "Transfer to account " + std::to_string(to_acc));
            add_transaction(to_acc, "TRANSFER_IN", amount, "Transfer from account " + std::to_string(from_acc));

            if (mysql_query(db.conn, "COMMIT")) throw std::runtime_error(mysql_error(db.conn));
        } catch (const std::exception &e) {
            // rollback
            mysql_query(db.conn, "ROLLBACK");
            throw;
        }
    }

    // Show account summary (incl. recent transactions)
    void show_account(int account_id) {
        Account a = get_account(account_id);
        std::ostringstream q;
        q << "SELECT c.name, c.email, c.phone FROM customers c JOIN accounts a ON a.customer_id = c.customer_id WHERE a.account_id=" << account_id << " LIMIT 1";
        if (mysql_query(db.conn, q.str().c_str())) throw std::runtime_error(mysql_error(db.conn));
        MYSQL_RES* res = mysql_store_result(db.conn);
        string name="N/A", email="N/A", phone="N/A";
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row) {
                name = row[0] ? row[0] : "N/A";
                email = row[1] ? row[1] : "N/A";
                phone = row[2] ? row[2] : "N/A";
            }
            mysql_free_result(res);
        }

        cout << "---- Account Summary ----\n";
        cout << "Account ID: " << a.id << "\n";
        cout << "Customer: " << name << " (Email: " << email << ", Phone: " << phone << ")\n";
        cout << "Account Type: " << a.account_type << "\n";
        cout << "Balance: " << std::fixed << std::setprecision(2) << a.balance << "\n";
        cout << "Recent transactions:\n";

        std::ostringstream qtx;
        qtx << "SELECT transaction_id, tx_type, amount, timestamp, remarks FROM transactions WHERE account_id=" << account_id << " ORDER BY timestamp DESC LIMIT 10";
        if (mysql_query(db.conn, qtx.str().c_str())) throw std::runtime_error(mysql_error(db.conn));
        MYSQL_RES *txres = mysql_store_result(db.conn);
        if (txres) {
            MYSQL_ROW row;
            cout << std::left << std::setw(6) << "ID" << std::setw(14) << "Type" << std::setw(12) << "Amount" << std::setw(22) << "Timestamp" << "Remarks\n";
            cout << "-----------------------------------------------------------------\n";
            while ((row = mysql_fetch_row(txres))) {
                cout << std::left << std::setw(6) << (row[0]?row[0]:"") 
                     << std::setw(14) << (row[1]?row[1]:"") 
                     << std::setw(12) << (row[2]?row[2]:"")
                     << std::setw(22) << (row[3]?row[3]:"")
                     << (row[4]?row[4]:"") << "\n";
            }
            mysql_free_result(txres);
        }
    }

    // Utility: list customers (simple)
    void list_customers() {
        if (mysql_query(db.conn, "SELECT customer_id, name, email, phone, created_at FROM customers ORDER BY customer_id DESC LIMIT 20")) throw std::runtime_error(mysql_error(db.conn));
        MYSQL_RES *res = mysql_store_result(db.conn);
        if (!res) throw std::runtime_error("No customers");
        cout << "ID\tName\tEmail\tPhone\tCreated\n";
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            cout << (row[0]?row[0]:"") << "\t" << (row[1]?row[1]:"") << "\t" << (row[2]?row[2]:"") << "\t" << (row[3]?row[3]:"") << "\t" << (row[4]?row[4]:"") << "\n";
        }
        mysql_free_result(res);
    }
};

// ---------- CLI ----------
void print_menu() {
    cout << "\n=== Simple Banking System ===\n";
    cout << "1. Create customer\n";
    cout << "2. Create account\n";
    cout << "3. Deposit\n";
    cout << "4. Withdraw\n";
    cout << "5. Transfer\n";
    cout << "6. Show account\n";
    cout << "7. List customers\n";
    cout << "0. Exit\n";
    cout << "Choose: ";
}

int main() {
    try {
        BankingService svc;
        int choice = -1;
        while (true) {
            print_menu();
            if (!(cin >> choice)) {
                cin.clear();
                string junk; std::getline(cin, junk);
                cout << "Invalid input\n";
                continue;
            }
            if (choice == 0) break;

            switch (choice) {
                case 1: {
                    cout << "Enter name: "; {
                        string name; std::getline(cin >> std::ws, name);
                        cout << "Email: "; string email; std::getline(cin, email);
                        cout << "Phone: "; string phone; std::getline(cin, phone);
                        int cid = svc.create_customer(name, email, phone);
                        cout << "Created customer id: " << cid << "\n";
                    }
                    break;
                }
                case 2: {
                    cout << "Enter customer id: "; int cid; cin >> cid;
                    cout << "Account type (SAVINGS/CURRENT): "; string atype; cin >> atype;
                    cout << "Initial deposit: "; double amt; cin >> amt;
                    int aid = svc.create_account(cid, atype, amt);
                    cout << "Created account id: " << aid << "\n";
                    break;
                }
                case 3: {
                    cout << "Account id: "; int aid; cin >> aid;
                    cout << "Amount to deposit: "; double amt; cin >> amt;
                    svc.deposit(aid, amt);
                    cout << "Deposit successful\n";
                    break;
                }
                case 4: {
                    cout << "Account id: "; int aid; cin >> aid;
                    cout << "Amount to withdraw: "; double amt; cin >> amt;
                    svc.withdraw(aid, amt);
                    cout << "Withdrawal successful\n";
                    break;
                }
                case 5: {
                    cout << "From Account id: "; int a1; cin >> a1;
                    cout << "To Account id: "; int a2; cin >> a2;
                    cout << "Amount: "; double amt; cin >> amt;
                    svc.transfer(a1, a2, amt);
                    cout << "Transfer successful\n";
                    break;
                }
                case 6: {
                    cout << "Account id: "; int aid; cin >> aid;
                    svc.show_account(aid);
                    break;
                }
                case 7: {
                    svc.list_customers();
                    break;
                }
                default:
                    cout << "Unknown choice\n";
                    break;
            }
        }

        cout << "Goodbye\n";
    } catch (const std::exception &ex) {
        cout << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
