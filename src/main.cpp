#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

// Educational single-process database. The WAL is the authoritative storage;
// rows and secondary indexes are reconstructed by replay at startup.
struct Token { std::string value; bool quoted = false; };
using Row = std::vector<std::string>;
struct Table {
    Row columns, types;
    std::vector<Row> rows;
    std::map<size_t, std::unordered_map<std::string, std::vector<size_t>>> indexes;
};
struct StorageError : std::runtime_error { using std::runtime_error::runtime_error; };
static void require(bool ok, const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}
static uint32_t checksum(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char ch : s) { h ^= ch; h *= 16777619u; }
    return h;
}
static std::vector<Token> lex(const std::string& s) {
    std::vector<Token> tokens;
    for (size_t i = 0; i < s.size();) {
        unsigned char ch = s[i];
        if (std::isspace(ch)) { ++i; continue; }
        if (ch == '\'') {
            ++i; std::string value; bool closed = false;
            while (i < s.size()) {
                if (s[i] == '\'') {
                    ++i;
                    if (i < s.size() && s[i] == '\'') { value += '\''; ++i; }
                    else { closed = true; break; }
                } else value += s[i++];
            }
            require(closed, "Unterminated string"); tokens.push_back({value, true});
        } else if (std::string("(),=*;").find(ch) != std::string::npos) {
            tokens.push_back({std::string(1, s[i++]), false});
        } else {
            size_t start = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) &&
                   std::string("(),=*;'").find(s[i]) == std::string::npos) ++i;
            require(i > start, "Unexpected character");
            tokens.push_back({s.substr(start, i-start), false});
        }
    }
    if (!tokens.empty() && tokens.back().value == ";" && !tokens.back().quoted) tokens.pop_back();
    return tokens;
}
class Parser {
    std::vector<Token> tokens; size_t pos = 0;
public:
    explicit Parser(const std::string& s): tokens(lex(s)) {}
    bool done() const { return pos == tokens.size(); }
    bool eat(const std::string& s) {
        if (!done() && !tokens[pos].quoted && tokens[pos].value == s) { ++pos; return true; }
        return false;
    }
    void expect(const std::string& s) { require(eat(s), "Expected " + s); }
    Token next() { require(!done(), "Unexpected end of command"); return tokens[pos++]; }
    std::string id() {
        auto t = next();
        require(!t.quoted && std::regex_match(t.value, std::regex("[A-Za-z_][A-Za-z_0-9]*")), "Invalid identifier");
        return t.value;
    }
    void end() { require(done(), "Unexpected trailing tokens"); }
};
static std::string typed(Token t, const std::string& type) {
    if (type == "TEXT") { require(t.quoted, "TEXT values must use single quotes"); return t.value; }
    require(!t.quoted && std::regex_match(t.value, std::regex("[+-]?[0-9]+")), "Invalid INT value");
    try { return std::to_string(std::stoll(t.value)); }
    catch (...) { throw std::runtime_error("INT out of signed 64-bit range"); }
}
static std::string csv(const Row& row) {
    std::string result;
    for (const auto& v : row) {
        if (!result.empty()) result += ',';
        result += '"';
        for (char c : v) { if (c == '"') result += '"'; result += c; }
        result += '"';
    }
    return result;
}
class Database {
    std::map<std::string, Table> tables;
    std::filesystem::path root;
    int fd = -1;
    Table& table(const std::string& name) {
        auto it = tables.find(name); require(it != tables.end(), "Unknown table"); return it->second;
    }
    static size_t column(const Table& t, const std::string& name) {
        auto it = std::find(t.columns.begin(), t.columns.end(), name);
        require(it != t.columns.end(), "Unknown column"); return it - t.columns.begin();
    }
    void log(const std::string& command, bool replay) {
        if (replay) return;
        std::string record = std::to_string(command.size()) + " " + std::to_string(checksum(command)) + "\n" + command + "\n";
        size_t offset = 0;
        while (offset < record.size()) {
            ssize_t n = ::write(fd, record.data()+offset, record.size()-offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw StorageError("WAL write failed; restart required");
            offset += static_cast<size_t>(n);
        }
        if (::fsync(fd) != 0) throw StorageError("WAL fsync failed; restart required");
        // Deterministic crash injection after durable logging, before applying.
        if (std::getenv("QUICKDB_CRASH_AFTER_WAL")) ::_exit(86);
    }
    void recover() {
        std::ifstream in(root / "database.wal", std::ios::binary);
        uint64_t valid = 0;
        while (true) {
            std::string header;
            if (!std::getline(in, header)) break;
            if (in.eof()) break; // torn header, not a complete record
            std::istringstream h(header); uint64_t length; uint32_t expected; std::string extra;
            require(bool(h >> length >> expected) && !(h >> extra) && length <= 1048576, "Corrupt WAL header");
            std::string command(length, '\0');
            in.read(command.data(), static_cast<std::streamsize>(length));
            if (static_cast<uint64_t>(in.gcount()) != length) break;
            char terminator;
            if (!in.get(terminator)) break;
            require(terminator == '\n' && checksum(command) == expected, "Corrupt WAL record");
            execute(command, true);
            valid = static_cast<uint64_t>(in.tellg());
        }
        if (::ftruncate(fd, static_cast<off_t>(valid)) != 0 || ::fsync(fd) != 0)
            throw StorageError("Cannot repair WAL tail");
    }
public:
    explicit Database(const std::filesystem::path& directory): root(directory) {
        std::filesystem::create_directories(root);
        fd = ::open((root / "database.wal").c_str(), O_CREAT|O_RDWR|O_APPEND, 0600);
        if (fd < 0) throw StorageError("Cannot open WAL");
        try {
            if (::flock(fd, LOCK_EX|LOCK_NB) != 0) throw StorageError("Database already open by another process");
            int dirfd = ::open(root.c_str(), O_RDONLY|O_DIRECTORY);
            if (dirfd < 0) throw StorageError("Cannot open database directory");
            int result = ::fsync(dirfd); ::close(dirfd);
            if (result != 0) throw StorageError("Cannot sync database directory");
            recover();
        } catch (...) { ::close(fd); fd = -1; throw; }
    }
    ~Database() { if (fd >= 0) ::close(fd); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    void execute(const std::string& command, bool replay = false) {
        require(command.size() <= 1048576, "Command exceeds 1 MiB limit");
        Parser p(command);
        if (p.done()) return;
        if (p.eat("CREATE")) {
            if (p.eat("TABLE")) {
                auto name = p.id(); require(!tables.count(name), "Table already exists");
                p.expect("("); Table t;
                do {
                    auto col = p.id(), type = p.id();
                    require(type == "INT" || type == "TEXT", "Type must be INT or TEXT");
                    require(std::find(t.columns.begin(), t.columns.end(), col) == t.columns.end(), "Duplicate column");
                    t.columns.push_back(col); t.types.push_back(type);
                } while(p.eat(","));
                p.expect(")"); p.end(); log(command, replay); tables.emplace(name, std::move(t));
            } else {
                p.expect("INDEX"); p.expect("ON"); auto& t = table(p.id());
                p.expect("("); auto col = column(t,p.id()); p.expect(")"); p.end();
                require(!t.indexes.count(col), "Index already exists");
                std::unordered_map<std::string,std::vector<size_t>> index;
                for (size_t i=0; i<t.rows.size(); ++i) index[t.rows[i][col]].push_back(i);
                log(command,replay); t.indexes.emplace(col,std::move(index));
            }
        } else if (p.eat("INSERT")) {
            p.expect("INTO"); auto& t = table(p.id()); p.expect("VALUES"); p.expect("(");
            Row row;
            do {
                require(row.size()<t.columns.size(), "Too many values");
                row.push_back(typed(p.next(), t.types[row.size()]));
            } while(p.eat(","));
            p.expect(")"); p.end(); require(row.size()==t.columns.size(), "Wrong value count");
            log(command,replay);
            t.rows.push_back(row);
            for(auto& index:t.indexes) index.second[row[index.first]].push_back(t.rows.size()-1);
        } else if (p.eat("SELECT")) {
            require(!replay,"Unexpected query in WAL");
            p.expect("*"); p.expect("FROM"); auto& t=table(p.id());
            bool filtered=p.eat("WHERE"); size_t col=0; std::string value;
            if(filtered) { col=column(t,p.id()); p.expect("="); value=typed(p.next(),t.types[col]); }
            bool scan=p.eat("SCAN"); p.end();
            auto start=std::chrono::steady_clock::now();
            std::vector<size_t> matches; size_t examined=0;
            auto index=t.indexes.find(col);
            bool indexed=filtered && !scan && index!=t.indexes.end();
            if(indexed) {
                auto it=index->second.find(value);
                if(it!=index->second.end()) matches=it->second;
                examined=matches.size();
            } else {
                for(size_t i=0;i<t.rows.size();++i) {
                    ++examined;
                    if(!filtered || t.rows[i][col]==value) matches.push_back(i);
                }
            }
            auto nanos=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            std::cout<<csv(t.columns)<<'\n';
            for(auto i:matches) std::cout<<csv(t.rows[i])<<'\n';
            std::cout<<"END rows="<<matches.size()<<" examined="<<examined<<" plan="<<(indexed?"index":"scan")<<" query_ns="<<nanos<<std::endl;
            return;
        } else if (p.eat("EXPORT")) {
            require(!replay,"Unexpected export in WAL");
            auto name=p.id(); auto& t=table(name); p.end();
            std::ofstream out(root/(name+".csv"));
            out<<csv(t.columns)<<'\n'; for(const auto& row:t.rows) out<<csv(row)<<'\n';
            out.close(); require(bool(out),"CSV export failed");
        } else throw std::runtime_error("Unknown command (keywords are uppercase)");
        if(!replay) std::cout<<"OK"<<std::endl;
    }
};
int main(int argc,char** argv) {
    if(argc!=2) { std::cerr<<"Usage: quickdb DATABASE_DIRECTORY\n"; return 2; }
    try {
        Database db(argv[1]); std::string line;
        while(std::getline(std::cin,line)) {
            if(line=="QUIT" || line=="QUIT;") break;
            try { db.execute(line); }
            catch(const StorageError&) { throw; }
            catch(const std::bad_alloc&) { throw; }
            catch(const std::exception& e) { std::cout<<"ERROR "<<e.what()<<std::endl; }
        }
    } catch(const std::exception& e) { std::cerr<<"FATAL "<<e.what()<<'\n'; return 1; }
}
