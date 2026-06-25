#include <parameda/expr.hpp>

#include <stdexcept>

namespace parameda {

namespace {

bool is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Trim leading/trailing *literal* whitespace of an argument template (the
// template text only — never whitespace produced by a placeholder value). So
// `$join{ a , b }` sees args "a","b", while `$f{${x}}` leaves ${x}'s value
// untouched.
void trim_expr(Expr& e) {
    if (!e.empty() && e.front().kind == Segment::Kind::Text) {
        std::string& t = e.front().text;
        std::size_t p = 0;
        while (p < t.size() && is_ws(t[p])) ++p;
        t.erase(0, p);
        if (t.empty()) e.erase(e.begin());
    }
    if (!e.empty() && e.back().kind == Segment::Kind::Text) {
        std::string& t = e.back().text;
        std::size_t q = t.size();
        while (q > 0 && is_ws(t[q - 1])) --q;
        t.erase(q);
        if (t.empty()) e.pop_back();
    }
}

class Parser {
public:
    explicit Parser(const std::string& src) : s_(src) {}

    Expr parse() { return parse_segments(""); }

private:
    bool eof() const { return i_ >= s_.size(); }
    char cur() const { return s_[i_]; }

    // Parse a run of segments until end-of-input or, when `stops` is non-empty,
    // a top-level character in `stops` (`,` / `}` inside an argument list).
    Expr parse_segments(const std::string& stops) {
        Expr out;
        std::string text;
        auto flush = [&]() {
            if (!text.empty()) {
                out.push_back(Segment{Segment::Kind::Text, text, {}});
                text.clear();
            }
        };
        while (!eof()) {
            char c = cur();
            if (!stops.empty() && stops.find(c) != std::string::npos) break;
            if (c == '\\') { // escape: \X -> X (\\ at EOF stays a backslash)
                if (i_ + 1 < s_.size()) {
                    text += s_[i_ + 1];
                    i_ += 2;
                } else {
                    text += '\\';
                    i_ += 1;
                }
                continue;
            }
            if (c == '$') {
                // A placeholder is `$` ident? `{`. Look ahead; if no `{`
                // follows, this `$` is ordinary literal text.
                std::size_t j = i_ + 1;
                while (j < s_.size() && is_ident(s_[j])) ++j;
                if (j < s_.size() && s_[j] == '{') {
                    flush();
                    Action act;
                    act.name = s_.substr(i_ + 1, j - (i_ + 1));
                    i_ = j + 1; // past '{'
                    act.args = parse_args();
                    if (eof() || cur() != '}')
                        throw std::runtime_error(
                            "parameda: unterminated placeholder '$" + act.name +
                            "{'");
                    i_ += 1; // past '}'
                    out.push_back(Segment{Segment::Kind::Action, "", std::move(act)});
                    continue;
                }
                text += '$';
                i_ += 1;
                continue;
            }
            text += c;
            i_ += 1;
        }
        flush();
        return out;
    }

    // Parse a comma-separated argument list. Precondition: i_ is just past `{`.
    // Leaves i_ at the closing `}` (or EOF, which the caller reports).
    std::vector<Expr> parse_args() {
        std::vector<Expr> args;
        if (!eof() && cur() == '}') return args; // empty: `$now{}`
        while (true) {
            Expr arg = parse_segments(",}");
            trim_expr(arg);
            args.push_back(std::move(arg));
            if (eof()) break;
            if (cur() == ',') { i_ += 1; continue; }
            break; // '}'
        }
        return args;
    }

    const std::string& s_;
    std::size_t i_ = 0;
};

} // namespace

Expr parse_template(const std::string& src) { return Parser(src).parse(); }

} // namespace parameda
