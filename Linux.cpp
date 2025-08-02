// password_manager.cpp
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <random>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#define getch _getch
#else
#include <unistd.h>
#include <termios.h>
#endif

using namespace std;
namespace fs = std::filesystem;

unordered_map<string, string> password_db;
mutex db_mutex;
string key;
unsigned long long key_value = 0;
int fake_interval = 0;
fs::path data_file;

void clear_screen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

#ifdef _WIN32
int getch_wrapper() {
    return _getch();
}
#else
int getch_wrapper() {
    struct termios oldt, newt;
    int ch;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}
#endif

int get_arrow_input() {
#ifdef _WIN32
    int ch = getch_wrapper();
    if (ch == 224 || ch == 0) {
        ch = getch_wrapper();
        if (ch == 72) return -1; // Up
        else if (ch == 80) return 1; // Down
    } else if (ch == 13) return 0; // Enter
    else if (ch == 8) return 2; // Backspace
#else
    int ch = getch_wrapper();
    if (ch == '\033') {
        getch_wrapper(); // skip [
        ch = getch_wrapper();
        if (ch == 'A') return -1;
        else if (ch == 'B') return 1;
    } else if (ch == '\n') return 0;
    else if (ch == 127) return 2;
#endif
    return -2;
}

string get_user_folder() {
#ifdef _WIN32
    char path[MAX_PATH];
    SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, path);
    return string(path);
#else
    const char* home = getenv("HOME");
    return home ? string(home) : ".";
#endif
}

string get_hidden_input() {
    string input;
#ifdef _WIN32
    char ch;
    while ((ch = _getch()) != '\r') {
        if (ch == '\b' && !input.empty()) {
            input.pop_back();
            cout << "\b \b";
        } else if (isprint(ch)) {
            input += ch;
            cout << '*';
        }
    }
#else
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    getline(cin, input);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif
    return input;
}

// === Encode/Decode ===
unsigned long long calculate_key_value(const string& key) {
    unsigned long long kv = 1;
    auto char_value = [](char ch) -> unsigned long long {
        ch = tolower((unsigned char)ch);
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 1;
        switch (ch) {
            case 'ç': return 27;
            case 'ğ': return 28;
            case 'ı': return 29;
            case 'ö': return 30;
            case 'ş': return 31;
            case 'ü': return 32;
            default: return 1;
        }
    };

    if (key.size() < 2) for (char ch : key) kv *= char_value(ch);
    else {
        for (size_t i = 0; i < key.size() - 2; ++i) kv *= char_value(key[i]);
        kv *= char_value(key[key.size() - 2]) * char_value(key[key.size() - 1]);
    }

    for (char ch : key) if (isdigit(ch) && ch != '0') { kv /= (ch - '0'); break; }
    for (char ch : key) if (isupper(ch)) {
        int pos = tolower(ch) - 'a' + 1;
        if (pos > 0) kv = (kv > pos) ? kv - pos : 0;
        break;
    }

    if (key.length() >= 5) kv += 50;
    return kv;
}

string insert_fake_data(const string& text, int interval, mt19937& rng) {
    if (interval <= 0) return text;
    uniform_int_distribution<size_t> dist(0, text.size() - 1);
    string result;
    int count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        result += text[i];
        count++;
        if (count == interval) {
            result += text[dist(rng)];
            count = 0;
        }
    }
    return result;
}

string remove_fake_data(const string& text, int interval) {
    if (interval <= 0) return text;
    string result;
    int count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (count == interval) { count = 0; continue; }
        result += text[i];
        count++;
    }
    return result;
}

string encode(const string& text, unsigned long long kv, int fi) {
    vector<char> chars(text.begin(), text.end());
    if (chars.empty()) return "";
    size_t len = chars.size();
    while (kv > 0) {
        size_t from = (kv - 1) % len;
        size_t to = (from + kv) % len;
        char ch = chars[to];
        chars.erase(chars.begin() + to);
        chars.insert(chars.begin() + from, ch);
        kv--;
    }
    string result(chars.begin(), chars.end());
    mt19937 rng((unsigned)time(nullptr));
    return insert_fake_data(result, fi, rng);
}

string decode(const string& text, unsigned long long kv, int fi) {
    string cleaned = remove_fake_data(text, fi);
    vector<char> chars(cleaned.begin(), cleaned.end());
    size_t len = chars.size();
    unsigned long long n = 1;
    while (n <= kv) {
        size_t from = (n - 1) % len;
        size_t to = (from + n) % len;
        char ch = chars[from];
        chars.erase(chars.begin() + from);
        chars.insert(chars.begin() + to, ch);
        n++;
    }
    return string(chars.begin(), chars.end());
}

// === Save/Load ===
void save_data() {
    lock_guard<mutex> lock(db_mutex);
    ofstream out(data_file);
    for (auto& [enc_name, enc_pass] : password_db)
        out << enc_name << "," << enc_pass << "\n";
}

void reload_data() {
    lock_guard<mutex> lock(db_mutex);
    password_db.clear();
    ifstream in(data_file);
    string line;
    while (getline(in, line)) {
        size_t sep = line.find(',');
        if (sep != string::npos) {
            string enc_name = line.substr(0, sep);
            string enc_pass = line.substr(sep + 1);
            password_db[enc_name] = enc_pass;
        }
    }
}

// === GUI ===
void draw_menu(const vector<string>& options, int selected) {
    clear_screen();
    cout << "==== Password Manager ====\n";
    for (size_t i = 0; i < options.size(); ++i) {
        if ((int)i == selected) cout << "> ";
        else cout << "  ";
        cout << options[i] << "\n";
    }
}

void add_password() {
    string name, pw;
    cout << "Account name: ";
    getline(cin, name);
    cout << "Password: ";
    pw = get_hidden_input();
    string enc_name = encode(name, key_value, fake_interval);
    string enc_pass = encode(pw, key_value, fake_interval);
    password_db[enc_name] = enc_pass;
    save_data();
    reload_data();
    cout << "\nSaved. Press any key to continue.\n";
    getch_wrapper();
}

void show_password_gui() {
    vector<pair<string, string>> entries;
    for (auto& [k, v] : password_db) entries.emplace_back(k, v);
    if (entries.empty()) {
        cout << "No entries. Press any key.\n"; getch_wrapper(); return;
    }
    int selected = 0;
    while (true) {
        clear_screen();
        cout << "Select Account to Show Password:\n";
        for (int i = 0; i < (int)entries.size(); ++i) {
            string dec = decode(entries[i].first, key_value, fake_interval);
            if (i == selected) cout << "> ";
            else cout << "  ";
            cout << dec << "\n";
        }
        int dir = get_arrow_input();
        if (dir == -1 && selected > 0) selected--;
        else if (dir == 1 && selected < (int)entries.size() - 1) selected++;
        else if (dir == 0) {
            string pw = decode(entries[selected].second, key_value, fake_interval);
            cout << "\nPassword: " << pw << "\nPress any key.\n";
            getch_wrapper();
            break;
        } else if (dir == 2) break;
    }
}

void remove_password() {
    vector<pair<string, string>> entries;
    for (auto& [k, v] : password_db)
        entries.emplace_back(k, v);

    if (entries.empty()) {
        cout << "No entries to remove. Press any key."; getch_wrapper(); return;
    }

    int selected = 0;
    while (true) {
        clear_screen();
        cout << "Select account to remove:\n";
        for (int i = 0; i < (int)entries.size(); ++i) {
            string dec = decode(entries[i].first, key_value, fake_interval);
            if (i == selected) cout << "> ";
            else cout << "  ";
            cout << dec << "\n";
        }
        int dir = get_arrow_input();
        if (dir == -1 && selected > 0) selected--;
        else if (dir == 1 && selected < (int)entries.size() - 1) selected++;
        else if (dir == 0) {
            string name_enc = entries[selected].first;
            string name_dec = decode(name_enc, key_value, fake_interval);
            clear_screen();
            cout << "Are you sure you want to remove '" << name_dec << "'?\n";
            cout << "You can't revert it back.\n";
            cout << "Press Enter to confirm, Backspace to cancel.\n";
            while (true) {
                int confirm = get_arrow_input();
                if (confirm == 0) {
                    password_db.erase(name_enc);
                    save_data();
                    reload_data();
                    cout << "Removed. Press any key to continue."; getch_wrapper();
                    return;
                } else if (confirm == 2) {
                    return;
                }
            }
        } else if (dir == 2) return;
    }
}

void run_transfer() {
    clear_screen();
    cout << "Starting transfer script...\n\n";
#ifdef _WIN32
    system("python transfer.py");
#else
    system("python3 transfer.py");
#endif
    cout << "\nTransfer finished or exited. Press any key to continue.\n";
    getch_wrapper();
}

void run_gui_menu() {
    vector<string> options = {"Add", "Show", "Remove", "Transfer", "Exit"};
    int selected = 0;
    while (true) {
        draw_menu(options, selected);
        int dir = get_arrow_input();
        if (dir == -1 && selected > 0) selected--;
        else if (dir == 1 && selected < (int)options.size() - 1) selected++;
        else if (dir == 0) {
            string choice = options[selected];
            if (choice == "Add") add_password();
            else if (choice == "Show") show_password_gui();
            else if (choice == "Remove") remove_password();
            else if (choice == "Transfer") run_transfer();
            else if (choice == "Exit") break;
        }
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    fs::path folder = fs::path(get_user_folder()) / "passwd";
    fs::create_directories(folder);
    data_file = folder / "data.txt";

    cout << "Enter master key: ";
    key = get_hidden_input();
    clear_screen();
    key_value = calculate_key_value(key);
    string kvs = to_string(key_value);
    int half = max(1, (int)kvs.size() / 2);
    fake_interval = 0;
    for (int i = (int)kvs.size() - half; i < (int)kvs.size(); ++i)
        if (isdigit(kvs[i])) fake_interval += kvs[i] - '0';

    reload_data();
    run_gui_menu();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
