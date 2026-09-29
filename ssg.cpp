#include <cmark.h>
#include <toml.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

static std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "error: cannot open " << path << "\n";
        return "";
    }
    // Read the entire file into the output string
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
}

// ---------------------------------------------------------------------------
// Frontmatter parsing
// Expects +++ delimiters with TOML content between them.
// Returns the parsed key/value table and the remaining markdown content.
// Updates content, chopping off the frontmatter portion
// ---------------------------------------------------------------------------

static toml::table parse_frontmatter(std::string& content) {
    if (content.substr(0, 3) != "+++")
        return toml::table{};

    std::size_t closing_pos = content.find("\n+++", 3);
    if (closing_pos == std::string::npos)
        return toml::table{};

    std::string frontmatter_block = content.substr(4, closing_pos - 4);
    content = content.substr(closing_pos + 4);
    if (!content.empty() && content.front() == '\n')
        content.erase(0, 1);

    return toml::parse(frontmatter_block);
}

// ---------------------------------------------------------------------------
// Template substitution
// Replaces every occurrence of {{key}} with value in place
// ---------------------------------------------------------------------------

static void replace_all(std::string& text, const std::string& target, const std::string& value) {
    std::size_t position = 0;
    while ((position = text.find(target, position)) != std::string::npos) {
        text.replace(position, target.size(), value);
        position += value.size();
    }
}

static void substitute(std::string& text, const std::string& key, const std::string& value) {
    replace_all(text, "{{" + key + "}}", value);
}

static void substitute_frontmatter(std::string& text, const toml::table& frontmatter) {
    for (auto&& [key, value] : frontmatter) {
        if (const auto* string_value = value.as_string())
            substitute(text, std::string(key), string_value->get());
    }
}

// ---------------------------------------------------------------------------
// Use cmark to convert markdown to html
// CMARK_OPT_UNSAFE keeps raw HTML in the output. Since cmark 0.29 the default is
// to strip it, which would silently drop any inline HTML written in an article.
// This is our own content, so there is no untrusted input to defend against.
// ---------------------------------------------------------------------------

static std::string markdown_to_html(const std::string& markdown) {
    // allocates char*, must free later
    char* raw_html = cmark_markdown_to_html(markdown.c_str(), markdown.size(), CMARK_OPT_UNSAFE);
    std::string result(raw_html);
    free(raw_html);
    return result;
}

// ---------------------------------------------------------------------------
// Math protection
//
// CommonMark treats a backslash before ASCII punctuation as an escape, so \\,
// \, \{ and \% do not survive markdown conversion, and * inside a formula turns
// into emphasis. Math is therefore lifted out before cmark sees the text,
// replaced by an inert alphanumeric placeholder, and spliced back afterwards for
// KaTeX to render in the browser.
// ---------------------------------------------------------------------------

struct MathSpan {
    std::string tex;
    bool display = false; // $$...$$ rather than $...$
};

static std::string html_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;";  break;
        case '>': escaped += "&gt;";  break;
        default:  escaped += character; break;
        }
    }
    return escaped;
}

// Trailing "END" keeps one placeholder from being a prefix of another.
static std::string math_placeholder(std::size_t index) {
    return "KTXMATH" + std::to_string(index) + "END";
}

static std::string extract_math(const std::string& markdown, std::vector<MathSpan>& spans) {
    std::string output;
    output.reserve(markdown.size());

    for (std::size_t index = 0; index < markdown.size();) {
        // A backslash escapes whatever follows, so \$ is a literal dollar sign.
        if (markdown[index] == '\\' && index + 1 < markdown.size()) {
            output += markdown[index];
            output += markdown[index + 1];
            index += 2;
            continue;
        }

        if (markdown[index] != '$') {
            output += markdown[index];
            ++index;
            continue;
        }

        const bool display          = index + 1 < markdown.size() && markdown[index + 1] == '$';
        const std::string delimiter = display ? "$$" : "$";
        const std::size_t body_start = index + delimiter.size();
        const std::size_t body_end   = markdown.find(delimiter, body_start);

        bool is_math = body_end != std::string::npos && body_end > body_start;
        // An unpaired $ in prose (a price, say) should stay literal, so inline
        // math is not allowed to run across a paragraph break.
        // find returns npos when absent, which is never < body_end.
        if (is_math && !display && markdown.find("\n\n", body_start) < body_end)
            is_math = false;

        if (!is_math) {
            output += markdown[index];
            ++index;
            continue;
        }

        MathSpan span;
        span.tex     = markdown.substr(body_start, body_end - body_start);
        span.display = display;
        output += math_placeholder(spans.size());
        spans.push_back(std::move(span));
        index = body_end + delimiter.size();
    }

    return output;
}

static void restore_math(std::string& html, const std::vector<MathSpan>& spans) {
    for (std::size_t index = 0; index < spans.size(); ++index) {
        // A span even for display math: cmark wraps the placeholder in a <p>, and
        // a block-level element inside <p> is invalid HTML. CSS blocks it instead.
        const std::string element =
            std::string("<span class=\"math ") + (spans[index].display ? "math-display" : "math-inline") +
            "\">" + html_escape(spans[index].tex) + "</span>";
        replace_all(html, math_placeholder(index), element);
    }
}

// ---------------------------------------------------------------------------
// Code blocks
//
// A fenced block is highlighted here, at build time, rather than by a script in
// the browser: the four languages this site writes about are all C-like enough to
// share one tokenizer, and Odin is in neither highlight.js nor Prism, so either
// would have wanted a hand-written grammar anyway. Doing it here costs the reader
// nothing — no library to download, no flash of unhighlighted code — and the
// output is plain spans that the palette colours like everything else.
//
// The lift-and-splice is the same shape as math, with one difference: only the
// *body* of the block is replaced by a placeholder, and the fence itself is left
// in the markdown. cmark therefore still decides what is a code block, what is
// inside a list item, and what the language class says, and this pass never has
// to reason about block structure. Inline `code` spans are lifted the same way.
//
// It has to run before extract_math and expand_sidenotes, because the languages
// use their syntax: $T is a polymorphic type parameter in Odin, and arr^[i] is an
// index through a pointer. Without this pass those would be lifted as a math span
// and a sidenote, and the code would arrive at cmark already mangled.
// ---------------------------------------------------------------------------

struct CodeSpan {
    std::string html; // already escaped, and already marked up if highlighted
};

static std::string code_placeholder(std::size_t index) {
    return "KTXCODE" + std::to_string(index) + "END";
}

// --- Language descriptions ---
//
// Flags rather than four separate lexers: the languages differ only in which of a
// handful of lexical features they have, and the shared loop below is the part
// that would otherwise be written four times.

struct Language {
    std::set<std::string> keywords;
    std::set<std::string> types;
    std::set<std::string> literals;   // true / nil / None — coloured as literals
    std::string string_prefixes;      // letters that bind to a following quote

    char line_comment      = '/';     // '/' for //, otherwise the character itself
    bool block_comment     = true;    // /* ... */
    bool nested_comment    = false;   // Odin nests them; C does not
    bool hash_directive    = false;   // #include, #load
    bool dollar_directive  = false;   // Odin's $T
    bool at_decorator      = false;   // Python's @decorator
    bool triple_quote      = false;   // Python's """ """
    bool backtick_string   = false;   // Odin's `raw string`
    bool quote_separator   = false;   // C++14's 1'000'000
};

static std::set<std::string> word_set(const std::string& words) {
    std::set<std::string> result;
    std::istringstream stream(words);
    for (std::string word; stream >> word;)
        result.insert(word);
    return result;
}

// Returns nullptr for a language with no description, which includes the ones
// named deliberately to opt out of highlighting.
static const Language* language_for(const std::string& name) {
    static const std::map<std::string, std::string> aliases = {
        {"c", "c"},        {"h", "c"},
        {"cpp", "cpp"},    {"c++", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"},
        {"hpp", "cpp"},    {"hxx", "cpp"},
        {"odin", "odin"},
        {"python", "python"}, {"py", "python"},
    };

    static const std::map<std::string, Language> table = [] {
        std::map<std::string, Language> languages;

        // The C keyword list is the base C++ extends, so it is built once.
        const std::string c_keywords =
            "auto break case const continue default do else enum extern for goto if inline "
            "register restrict return sizeof static struct switch typedef union volatile while "
            "_Alignas _Alignof _Atomic _Bool _Complex _Generic _Imaginary _Noreturn "
            "_Static_assert _Thread_local";
        const std::string c_types =
            "bool char double float int long short signed unsigned void "
            "size_t ssize_t ptrdiff_t intptr_t uintptr_t wchar_t "
            "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
            "FILE va_list";

        Language c;
        c.keywords        = word_set(c_keywords);
        c.types           = word_set(c_types);
        c.literals        = word_set("true false NULL");
        c.string_prefixes = "uUL8";
        c.hash_directive  = true;
        languages["c"] = c;

        Language cpp = c;
        cpp.keywords = word_set(
            c_keywords + " " +
            "alignas alignof and and_eq asm bitand bitor catch class compl concept consteval "
            "constexpr constinit const_cast co_await co_return co_yield decltype delete "
            "dynamic_cast explicit export friend mutable namespace new noexcept not not_eq "
            "operator or or_eq private protected public reinterpret_cast requires static_assert "
            "static_cast template this thread_local throw try typeid typename using virtual "
            "xor xor_eq");
        cpp.types = word_set(c_types + " char8_t char16_t char32_t nullptr_t");
        cpp.literals        = word_set("true false nullptr NULL");
        cpp.string_prefixes = "uURL8";
        cpp.quote_separator = true;
        languages["cpp"] = cpp;

        Language odin;
        odin.keywords = word_set(
            "asm auto_cast bit_field bit_set break case cast context continue defer distinct do "
            "dynamic else enum fallthrough for foreign if import in inline map matrix no_inline "
            "not_in or_break or_continue or_else or_return package proc return struct switch "
            "transmute typeid union using when where");
        odin.types = word_set(
            "bool b8 b16 b32 b64 byte rune string cstring rawptr any "
            "i8 i16 i32 i64 i128 int u8 u16 u32 u64 u128 uint uintptr "
            "i16le i32le i64le i128le u16le u32le u64le u128le "
            "i16be i32be i64be i128be u16be u32be u64be u128be "
            "f16 f32 f64 f16le f32le f64le f16be f32be f64be "
            "complex32 complex64 complex128 quaternion64 quaternion128 quaternion256");
        odin.literals        = word_set("true false nil");
        odin.nested_comment  = true;
        odin.hash_directive  = true;
        odin.dollar_directive = true;
        odin.backtick_string = true;
        languages["odin"] = odin;

        Language python;
        python.keywords = word_set(
            "and as assert async await break case class continue def del elif else except "
            "finally for from global if import in is lambda match nonlocal not or pass raise "
            "return try while with yield");
        // Python has no type syntax to speak of; these are the builtin constructors
        // that read as types at a glance, which is what the colour is for.
        python.types = word_set(
            "bool bytearray bytes complex dict float frozenset int list memoryview object "
            "set slice str tuple type");
        python.literals        = word_set("True False None NotImplemented Ellipsis");
        python.string_prefixes = "fFrRbBuU";
        python.line_comment    = '#';
        python.block_comment   = false;
        python.triple_quote    = true;
        python.at_decorator    = true;
        languages["python"] = python;

        return languages;
    }();

    const std::map<std::string, std::string>::const_iterator alias = aliases.find(name);
    if (alias == aliases.end())
        return nullptr;
    return &table.find(alias->second)->second;
}

// Named to say "leave this alone", as opposed to a language nobody has taught the
// generator yet — the second of those is worth a warning and the first is not.
static bool is_plain_language(const std::string& name) {
    return name == "text" || name == "txt" || name == "plain" || name == "none" ||
           name == "console" || name == "shell" || name == "sh" || name == "bash" ||
           name == "make" || name == "makefile" || name == "toml" || name == "md" ||
           name == "markdown" || name == "diff";
}

// --- The tokenizer ---

static bool is_ident_start(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
}

static bool is_ident_char(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

static std::string highlight(const std::string& source, const Language& language) {
    std::string output;
    output.reserve(source.size() * 2);

    const std::size_t length = source.size();

    // A token is a span around escaped text; a null class emits the text bare, so
    // punctuation and ordinary identifiers cost no markup at all.
    const auto emit = [&output](const std::string& text, const char* token) {
        if (token == nullptr) {
            output += html_escape(text);
            return;
        }
        output += "<span class=\"";
        output += token;
        output += "\">";
        output += html_escape(text);
        output += "</span>";
    };

    // Where the string opened at `start` ends, one past its closing quote. An
    // unterminated string stops at the newline rather than swallowing the rest of
    // the snippet, which is what makes a stray quote a local mistake.
    const auto scan_string = [&](std::size_t start) -> std::size_t {
        const char quote = source[start];
        if (language.triple_quote && start + 2 < length &&
            source[start + 1] == quote && source[start + 2] == quote) {
            const std::string fence(3, quote);
            const std::size_t close = source.find(fence, start + 3);
            return close == std::string::npos ? length : close + 3;
        }
        std::size_t scan = start + 1;
        while (scan < length && source[scan] != quote && source[scan] != '\n') {
            if (source[scan] == '\\' && scan + 1 < length)
                scan += 2;
            else
                ++scan;
        }
        return (scan < length && source[scan] == quote) ? scan + 1 : scan;
    };

    std::size_t index = 0;
    bool line_start   = true; // nothing but whitespace seen on this line yet

    while (index < length) {
        const char character = source[index];
        const char next      = index + 1 < length ? source[index + 1] : '\0';

        if (character == '\n') {
            output += character;
            ++index;
            line_start = true;
            continue;
        }
        if (character == ' ' || character == '\t' || character == '\r') {
            output += character;
            ++index;
            continue; // leaves line_start alone, so indentation does not end it
        }

        const bool line_comment = language.line_comment == '/'
                                      ? (character == '/' && next == '/')
                                      : (character == language.line_comment);
        if (line_comment) {
            std::size_t end = source.find('\n', index);
            if (end == std::string::npos)
                end = length;
            emit(source.substr(index, end - index), "tok-com");
            index = end;
            continue;
        }

        if (language.block_comment && character == '/' && next == '*') {
            std::size_t scan  = index + 2;
            int         depth = 1;
            while (scan < length && depth > 0) {
                if (language.nested_comment && source[scan] == '/' && scan + 1 < length &&
                    source[scan + 1] == '*') {
                    ++depth;
                    scan += 2;
                } else if (source[scan] == '*' && scan + 1 < length && source[scan + 1] == '/') {
                    --depth;
                    scan += 2;
                } else {
                    ++scan;
                }
            }
            emit(source.substr(index, scan - index), "tok-com");
            index      = scan;
            line_start = false;
            continue;
        }

        if (language.hash_directive && character == '#') {
            std::size_t scan = index + 1;
            while (scan < length && (is_ident_char(source[scan]) || source[scan] == '+'))
                ++scan;
            const std::string directive = source.substr(index, scan - index);
            emit(directive, "tok-pre");

            // <stdio.h> after an include is a path, not a pair of comparisons.
            if (directive == "#include" || directive == "#import") {
                std::size_t open = scan;
                while (open < length && (source[open] == ' ' || source[open] == '\t'))
                    ++open;
                const std::size_t close = source.find('>', open);
                if (open < length && source[open] == '<' && close != std::string::npos &&
                    source.find('\n', open) > close) {
                    output += source.substr(scan, open - scan);
                    emit(source.substr(open, close + 1 - open), "tok-str");
                    scan = close + 1;
                }
            }
            index      = scan;
            line_start = false;
            continue;
        }

        // Only at the head of a line, so Python's matrix-multiply a @ b is left as
        // the operator it is.
        if (language.at_decorator && character == '@' && line_start && is_ident_start(next)) {
            std::size_t scan = index + 1;
            while (scan < length && (is_ident_char(source[scan]) || source[scan] == '.'))
                ++scan;
            emit(source.substr(index, scan - index), "tok-pre");
            index      = scan;
            line_start = false;
            continue;
        }

        if (language.dollar_directive && character == '$' && is_ident_start(next)) {
            std::size_t scan = index + 1;
            while (scan < length && is_ident_char(source[scan]))
                ++scan;
            emit(source.substr(index, scan - index), "tok-typ");
            index      = scan;
            line_start = false;
            continue;
        }

        if (language.backtick_string && character == '`') {
            const std::size_t close = source.find('`', index + 1);
            const std::size_t end   = close == std::string::npos ? length : close + 1;
            emit(source.substr(index, end - index), "tok-str");
            index      = end;
            line_start = false;
            continue;
        }

        if (character == '"' || character == '\'') {
            const std::size_t end = scan_string(index);
            emit(source.substr(index, end - index), "tok-str");
            index      = end;
            line_start = false;
            continue;
        }

        const bool digit = std::isdigit(static_cast<unsigned char>(character)) != 0;
        if (digit || (character == '.' && std::isdigit(static_cast<unsigned char>(next)))) {
            std::size_t scan = index + (digit ? 0 : 1);
            while (scan < length) {
                const char current = source[scan];
                // Hex digits, exponent markers and suffixes are all identifier
                // characters, and 1_000 separates with an underscore.
                if (is_ident_char(current)) {
                    ++scan;
                } else if (current == '.' && scan + 1 < length &&
                           std::isdigit(static_cast<unsigned char>(source[scan + 1]))) {
                    ++scan; // a decimal point, and not the head of Odin's 0..<n
                } else if ((current == '+' || current == '-') && scan > index &&
                           std::string("eEpP").find(source[scan - 1]) != std::string::npos) {
                    ++scan; // a signed exponent
                } else if (language.quote_separator && current == '\'' && scan + 1 < length &&
                           std::isdigit(static_cast<unsigned char>(source[scan + 1]))) {
                    ++scan; // C++14's 1'000'000, before ' can look like a character
                } else {
                    break;
                }
            }
            emit(source.substr(index, scan - index), "tok-num");
            index      = scan;
            line_start = false;
            continue;
        }

        if (is_ident_start(character)) {
            std::size_t scan = index;
            while (scan < length && is_ident_char(source[scan]))
                ++scan;
            const std::string word = source.substr(index, scan - index);

            // A prefix binds to the quote that follows it, so f"{x}" and L"wide"
            // are one string token rather than a stray identifier and a string.
            if (scan < length && (source[scan] == '"' || source[scan] == '\'') &&
                word.size() <= 2 && !language.string_prefixes.empty() &&
                word.find_first_not_of(language.string_prefixes) == std::string::npos) {
                const std::size_t end = scan_string(scan);
                emit(source.substr(index, end - index), "tok-str");
                index      = end;
                line_start = false;
                continue;
            }

            const char* token = nullptr;
            if (language.keywords.count(word) != 0) {
                token = "tok-kw";
            } else if (language.types.count(word) != 0) {
                token = "tok-typ";
            } else if (language.literals.count(word) != 0) {
                token = "tok-num"; // a named literal is a literal
            } else {
                // Anything called is a function, which is as much as a lexer can
                // know without a symbol table — and is right often enough to read.
                std::size_t after = scan;
                while (after < length && (source[after] == ' ' || source[after] == '\t'))
                    ++after;
                if (after < length && source[after] == '(')
                    token = "tok-fn";
            }
            emit(word, token);
            index      = scan;
            line_start = false;
            continue;
        }

        emit(std::string(1, character), nullptr);
        ++index;
        line_start = false;
    }

    return output;
}

// --- Lifting code out of the markdown ---

// An opening fence: any indent, then three or more backticks or tildes. The
// indent is measured so the body can be dedented by the same amount, which is
// what lets a block inside a list item work.
static bool opens_fence(const std::string& line, char& fence_char, std::size_t& fence_len,
                        std::size_t& indent, std::string& info) {
    const std::size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos)
        return false;

    const char character = line[first];
    if (character != '`' && character != '~')
        return false;

    std::size_t run = 0;
    while (first + run < line.size() && line[first + run] == character)
        ++run;
    if (run < 3)
        return false;

    const std::string rest = line.substr(first + run);
    // A backtick fence may not carry a backtick in its info string. Without this
    // ``a`` at the head of a line would open a block that never closes.
    if (character == '`' && rest.find('`') != std::string::npos)
        return false;

    fence_char = character;
    fence_len  = run;
    indent     = first;
    info       = rest;
    return true;
}

static bool closes_fence(const std::string& line, char fence_char, std::size_t fence_len) {
    const std::size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos)
        return false;

    std::size_t run = 0;
    while (first + run < line.size() && line[first + run] == fence_char)
        ++run;
    if (run < fence_len)
        return false;

    return line.find_first_not_of(" \t\r", first + run) == std::string::npos;
}

// Turn the info string into the language name: its first word, lowercased.
static std::string fence_language(const std::string& info) {
    std::string name;
    std::istringstream stream(info);
    stream >> name;
    for (char& character : name)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return name;
}

static std::string render_code_block(const std::string& info, const std::string& text) {
    // The body arrives with a newline after its last line, and cmark writes one of
    // its own after the placeholder, so one of the two has to go or every block
    // ends on a blank line.
    const std::string source = (!text.empty() && text.back() == '\n')
                                   ? text.substr(0, text.size() - 1)
                                   : text;

    const std::string name = fence_language(info);
    if (name.empty())
        return html_escape(source);

    const Language* language = language_for(name);
    if (language == nullptr) {
        if (!is_plain_language(name))
            std::cerr << "warning: no highlighter for code language \"" << name
                      << "\"; the block is rendered plain\n";
        return html_escape(source);
    }
    return highlight(source, *language);
}

// Inline `code`, lifted for the same reason as a block and bounded to one line: a
// span cannot cross a paragraph break anyway, and the bound keeps an unmatched
// backtick from reaching for one several paragraphs down.
static std::string extract_inline_code(const std::string& line, std::vector<CodeSpan>& spans) {
    std::string output;
    output.reserve(line.size());

    const std::size_t length = line.size();
    for (std::size_t index = 0; index < length;) {
        if (line[index] == '\\' && index + 1 < length) {
            output += line[index];
            output += line[index + 1]; // \` is a literal backtick
            index += 2;
            continue;
        }
        if (line[index] != '`') {
            output += line[index];
            ++index;
            continue;
        }

        std::size_t run = 0;
        while (index + run < length && line[index + run] == '`')
            ++run;

        // The span closes on a run of exactly the same length, so ``a`b`` holds a
        // backtick rather than ending at it.
        std::size_t close = std::string::npos;
        for (std::size_t scan = index + run; scan < length;) {
            if (line[scan] != '`') {
                ++scan;
                continue;
            }
            std::size_t other = 0;
            while (scan + other < length && line[scan + other] == '`')
                ++other;
            if (other == run) {
                close = scan;
                break;
            }
            scan += other;
        }

        if (close == std::string::npos) {
            output.append(run, '`'); // never closed: leave the backticks as written
            index += run;
            continue;
        }

        std::string text = line.substr(index + run, close - (index + run));
        // CommonMark drops one space from each end when both are there and the
        // content is not all spaces, which is how `` ` `` holds a bare backtick.
        if (text.size() >= 2 && text.front() == ' ' && text.back() == ' ' &&
            text.find_first_not_of(' ') != std::string::npos)
            text = text.substr(1, text.size() - 2);

        CodeSpan span;
        span.html = html_escape(text);

        // The backticks go back in: cmark still decides this is a code span, and
        // only its contents have been swapped for something inert.
        output.append(run, '`');
        output += code_placeholder(spans.size());
        output.append(run, '`');
        spans.push_back(std::move(span));
        index = close + run;
    }

    return output;
}

static std::string extract_code(const std::string& markdown, std::vector<CodeSpan>& spans) {
    std::string output;
    output.reserve(markdown.size());

    char        fence_char = 0;
    std::size_t fence_len  = 0;
    std::size_t indent     = 0;
    std::string info;
    std::string body;
    bool        in_fence = false;

    // Strictly less than: a document ending in a newline has no final line, and
    // scanning one would put a blank line at the end of an unterminated block.
    for (std::size_t begin = 0; begin < markdown.size();) {
        std::size_t end = markdown.find('\n', begin);
        const bool  last = end == std::string::npos;
        if (last)
            end = markdown.size();

        const std::string line = markdown.substr(begin, end - begin);
        const bool has_newline = !last;

        if (in_fence) {
            if (closes_fence(line, fence_char, fence_len)) {
                CodeSpan span;
                span.html = render_code_block(info, body);
                // The placeholder carries the fence's own indent, so cmark strips
                // it back off exactly as it would have stripped the real body.
                output.append(indent, ' ');
                output += code_placeholder(spans.size());
                output += '\n';
                spans.push_back(std::move(span));

                output += line;
                if (has_newline)
                    output += '\n';
                in_fence = false;
                body.clear();
            } else {
                // Dedent by the fence's indent, no further than the line has.
                const std::size_t first = line.find_first_not_of(' ');
                const std::size_t drop  = std::min(indent, first == std::string::npos ? line.size() : first);
                body += line.substr(drop);
                body += '\n';
            }
        } else if (opens_fence(line, fence_char, fence_len, indent, info)) {
            output += line;
            if (has_newline)
                output += '\n';
            in_fence = true;
            body.clear();
        } else {
            output += extract_inline_code(line, spans);
            if (has_newline)
                output += '\n';
        }

        if (last)
            break;
        begin = end + 1;
    }

    // cmark closes an unterminated fence at the end of the document, so this does
    // too rather than dropping the block on the floor.
    if (in_fence) {
        CodeSpan span;
        span.html = render_code_block(info, body);
        output.append(indent, ' ');
        output += code_placeholder(spans.size());
        output += '\n';
        spans.push_back(std::move(span));
    }

    return output;
}

static void restore_code(std::string& html, const std::vector<CodeSpan>& spans) {
    for (std::size_t index = 0; index < spans.size(); ++index)
        replace_all(html, code_placeholder(index), spans[index].html);
}

// ---------------------------------------------------------------------------
// Sidenotes
//
// ^[note text] becomes the three elements a CSS-only sidenote needs: a numbered
// label, a checkbox, and the note itself. The number is drawn by a CSS counter,
// so nothing here has to track it and inserting a note mid-article renumbers the
// rest for free; the id only has to be unique within the page, which the running
// index gives. The checkbox is never seen — it is the state the narrow-viewport
// layout toggles when the number is tapped, which is what keeps this free of
// JavaScript.
//
// Unlike math this is a rewrite in place rather than a lift-and-splice: the note
// body is left where it is for cmark, so emphasis, code spans and links work
// inside a note. It runs after extract_math so that brackets in TeX (\left[ ...
// \right]) are already inert placeholders and cannot unbalance the matching
// below.
// ---------------------------------------------------------------------------

static std::string expand_sidenotes(const std::string& markdown) {
    std::string output;
    output.reserve(markdown.size());
    std::size_t count = 0;

    for (std::size_t index = 0; index < markdown.size();) {
        // \^ is a literal caret; cmark drops the backslash further down the line.
        if (markdown[index] == '\\' && index + 1 < markdown.size()) {
            output += markdown[index];
            output += markdown[index + 1];
            index += 2;
            continue;
        }

        const bool opens = markdown[index] == '^' && index + 1 < markdown.size() && markdown[index + 1] == '[';
        if (!opens) {
            output += markdown[index];
            ++index;
            continue;
        }

        // Count nesting, so a markdown link inside the note does not end it early.
        std::size_t depth = 1;
        std::size_t scan  = index + 2;
        for (; scan < markdown.size() && depth > 0; ++scan) {
            if (markdown[scan] == '\\')
                ++scan; // skip the escaped character, whatever it is
            else if (markdown[scan] == '[')
                ++depth;
            else if (markdown[scan] == ']')
                --depth;
        }

        // Never closed, or closed only after a paragraph break: leave the text
        // exactly as written. A note cannot span a blank line anyway, since cmark
        // would close the <p> in the middle of the span, and without the second
        // test a stray ^[ in prose would swallow every note after it up to
        // whichever ] happened to balance. find returns npos when absent, which
        // is never < scan.
        if (depth != 0 || markdown.find("\n\n", index) < scan - 1) {
            output += markdown[index];
            ++index;
            continue;
        }

        const std::string body = markdown.substr(index + 2, (scan - 1) - (index + 2));
        const std::string id   = "sn-" + std::to_string(++count);

        // No whitespace between the three: the narrow layout reveals the note
        // with input:checked + .sidenote, which needs them to stay adjacent.
        output += "<label for=\"" + id + "\" class=\"sidenote-number\"></label>";
        output += "<input type=\"checkbox\" id=\"" + id + "\" class=\"sidenote-toggle\">";
        output += "<span class=\"sidenote\">" + body + "</span>";
        index = scan;
    }

    return output;
}

// ---------------------------------------------------------------------------
// Images
//
// One pass over the rendered HTML that resolves every <img> against static/ and
// decides what the page should actually carry. Two things happen here, both of
// them things markdown has no way to say.
//
// Light/dark pairs. A diagram with a baked-in background glares on the opposite
// palette, so a figure may ship as kernels_light.png beside kernels_dark.png.
// The markdown names neither: it asks for /images/kernels.png, and both variants
// are emitted, classed light-only and dark-only for main.css to show and hide.
// <picture> with prefers-color-scheme is the obvious tool and the wrong one —
// that media query reads the OS preference and cannot see the in-page theme
// toggle, so it would strand the wrong variant on screen whenever the two
// disagree. Two tags and a class each is what follows the toggle.
//
// SVG inlining. An SVG referenced through <img> is a separate document: no CSS
// crosses that boundary, so stroke="currentColor" inside it resolves against the
// SVG's own initial colour — black — rather than the page's --fg, and the drawing
// glares in dark mode exactly like a baked-in PNG would. Splicing the file into
// the document is what puts it in reach of the cascade, and it is the whole
// reason to prefer an SVG diagram here. Doing it in the generator keeps the
// markdown reading ![alt](/images/x.svg) instead of carrying a screenful of path
// data.
//
// An exact filename hit wins over the pair, so a single-file image costs nothing
// and naming a variant outright still works. A name matching neither a file nor a
// complete pair is left alone and warned about rather than rewritten, since a
// rewrite could only turn one broken URL into a different broken one. Remote and
// relative src values are skipped: only a root-absolute path names a file here.
//
// Two limits, both fine at this size and neither worth pre-solving. An inlined
// SVG is not cached across pages, so a drawing used on several pages ships with
// each of them. And ids inside two SVGs on one page can collide, since nothing
// namespaces them — Excalidraw only emits ids under <defs>, which is why this has
// not bitten yet.
// ---------------------------------------------------------------------------

// kernels.png -> kernels_dark.png. A dot earlier in the path belongs to a
// directory name, not to the file, and an extensionless name takes the suffix at
// the end.
// True when the filename, before its extension, ends in `suffix`. The inverse of
// add_suffix, and used to read a naming convention back off a URL.
static bool has_name_suffix(const std::string& url, const std::string& suffix) {
    const std::size_t slash = url.rfind('/');
    std::size_t dot = url.rfind('.');

    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        dot = url.size();

    return dot >= suffix.size() && url.compare(dot - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string add_suffix(const std::string& url, const std::string& suffix) {
    const std::size_t slash = url.rfind('/');
    const std::size_t dot   = url.rfind('.');

    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return url + suffix;

    return url.substr(0, dot) + suffix + url.substr(dot);
}

// Merged into an existing class rather than appended as a second attribute, for
// the reason mark_current_link gives: a tag carrying two class attributes keeps
// only the first. The caller passes one opening tag, never a whole document —
// an SVG file has class attributes further down that must not be the ones found.
static std::string add_class(const std::string& tag, const std::string& name) {
    if (name.empty())
        return tag;

    const std::size_t existing = tag.find("class=\"");
    if (existing != std::string::npos)
        return tag.substr(0, existing + 7) + name + " " + tag.substr(existing + 7);

    // Just past the element name, so this works for <img and <svg alike.
    const std::size_t after_name = tag.find_first_of(" \t\n\r/>", 1);
    if (after_name == std::string::npos)
        return tag;

    return tag.substr(0, after_name) + " class=\"" + name + "\"" + tag.substr(after_name);
}

static std::string attribute_value(const std::string& tag, const std::string& name) {
    const std::size_t key = tag.find(name + "=\"");
    if (key == std::string::npos)
        return "";

    const std::size_t start = key + name.size() + 2;
    const std::size_t end   = tag.find('"', start);
    return end == std::string::npos ? "" : tag.substr(start, end - start);
}

// Splice an SVG file into the document in place of the <img> that referenced it.
// Returns an empty string if the file cannot stand in for the tag, leaving the
// caller to emit the original <img>: a missing or malformed drawing should show
// up as a broken image rather than as a hole in the page.
static std::string inline_svg(const std::filesystem::path& path, const std::string& tag,
                              const std::string& class_name) {
    std::ifstream file(path);
    if (!file)
        return "";

    std::string svg((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // An XML declaration or doctype is legal at the head of a standalone .svg
    // file and not legal in the middle of an HTML body.
    const std::size_t root = svg.find("<svg");
    if (root == std::string::npos)
        return "";
    svg.erase(0, root);

    const std::size_t root_end = svg.find('>');
    if (root_end == std::string::npos)
        return "";

    std::string root_tag = add_class(svg.substr(0, root_end + 1), class_name);

    // The alt text is the only accessible name the drawing had; an <svg> is not
    // labelled by anything unless told, so carry it across. An empty alt marks a
    // decorative image, which is exactly what aria-hidden says.
    const std::string alt = attribute_value(tag, "alt");
    if (attribute_value(root_tag, "role").empty() && attribute_value(root_tag, "aria-label").empty()) {
        const std::string label = alt.empty() ? " aria-hidden=\"true\"" : " role=\"img\" aria-label=\"" + alt + "\"";
        root_tag = root_tag.substr(0, 4) + label + root_tag.substr(4);
    }

    return root_tag + svg.substr(root_end + 1);
}

// --- Videos ---
//
// A video is written exactly like an image, ![alt](/videos/demo.mp4), and the
// extension alone is what turns the <img> cmark wrote into a <video>. Markdown
// has no video syntax, and borrowing the image one keeps the markdown free of
// hand-written tags whose attributes are easy to get subtly wrong.
//
// Everything else is read off files beside the video, the same way _auto and the
// _light/_dark pair are, so there is nothing to configure in the markdown:
//
//   demo_loop.mp4  autoplays muted and loops, with no controls — the silent clip
//                  at the top of an article. Browsers only autoplay muted video,
//                  and playsinline stops iOS from taking it fullscreen.
//   demo.mp4       has controls and downloads nothing until played, so a reader
//                  who never presses play pays only for the poster.
//   demo.jpg       the poster (.webp and .png also found). Optional for a loop,
//                  which shows its own first frame. Without one a click-to-play
//                  video would be an empty box, so it falls back to
//                  preload="metadata" and the build warns.
//   demo.av1.mp4   an AV1 encode, offered first. Particle footage is about the
//                  worst case for H.264 and AV1 is far smaller for it; the plain
//                  file stays as the fallback for browsers that cannot decode AV1,
//                  and a browser downloads only the source it picks.
//
// A remote URL gets a <video> too, but none of the sidecar lookups, since there is
// no directory to look in. That is what keeps moving videos to a CDN a matter of
// rewriting the src.
//
// No width or height is written: the element takes its size from the poster
// before the video loads, which is the same layout-shift behaviour every <img> on
// the site already has.

static bool is_video(const std::string& url) {
    const std::string extension = std::filesystem::path(url).extension().string();
    return extension == ".mp4" || extension == ".webm";
}

static std::string with_extension(const std::string& url, const std::string& extension) {
    return std::filesystem::path(url).replace_extension(extension).generic_string();
}

// Returns the original tag, with a warning, when a local video is missing: an
// <img> pointing at an .mp4 shows up as a broken image, where a silent autoplay
// <video> with nothing behind it would just be a hole in the page.
static std::string render_video(const std::string& tag, const std::string& source,
                                const std::filesystem::path& static_dir) {
    const bool local = source.front() == '/';
    const auto on_disk = [&](const std::string& url) {
        return local && std::filesystem::exists(static_dir / url.substr(1));
    };

    if (local && !on_disk(source)) {
        std::cerr << "warning: no video at " << source << "\n";
        return tag;
    }

    std::string poster;
    for (const char* extension : {".jpg", ".webp", ".png"}) {
        if (on_disk(with_extension(source, extension))) {
            poster = with_extension(source, extension);
            break;
        }
    }
    // Only a click-to-play video needs one. A loop starts on its own and shows its
    // first frame the moment it has one, which is all a poster would have shown;
    // a click-to-play video on iOS fetches nothing before the tap, so without a
    // poster it is an empty box.
    const bool loop = has_name_suffix(source, "_loop");
    if (local && poster.empty() && !loop)
        std::cerr << "warning: no poster for " << source << " (expected "
                  << with_extension(source, ".jpg") << ")\n";

    std::string video = "<video";
    if (loop)
        video += " autoplay muted loop playsinline";
    else
        video += poster.empty() ? " controls preload=\"metadata\" playsinline"
                                : " controls preload=\"none\" playsinline";

    if (!poster.empty())
        video += " poster=\"" + poster + "\"";

    // A video has no alt; aria-label is the equivalent. Unlike an inlined SVG, an
    // empty alt does not become aria-hidden — a player with controls is not
    // decorative, whatever its label says.
    const std::string alt = attribute_value(tag, "alt");
    if (!alt.empty())
        video += " aria-label=\"" + alt + "\"";

    const std::string av1  = add_suffix(source, ".av1");
    const std::string type = std::filesystem::path(source).extension() == ".webm" ? "video/webm" : "video/mp4";

    if (!on_disk(av1))
        return video + " src=\"" + source + "\"></video>";

    // The codec string is what lets a browser without an AV1 decoder skip the
    // first source unfetched. av01.0.08M.08: main profile, level 4.0 (1080p30),
    // 8-bit, which covers anything this site should be serving.
    return video + "><source src=\"" + av1 + "\" type='" + type + "; codecs=\"av01.0.08M.08\"'>"
                 + "<source src=\"" + source + "\" type=\"" + type + "\"></video>";
}

static std::string resolve_images(const std::string& html) {
    // Every path the generator touches is resolved against the working
    // directory, so a root-absolute URL is static/ plus that path.
    const std::filesystem::path static_dir = "static";

    std::string output;
    output.reserve(html.size());

    for (std::size_t index = 0; index < html.size();) {
        const std::size_t tag_start = html.find("<img ", index);
        const std::size_t tag_end   = tag_start == std::string::npos ? std::string::npos : html.find('>', tag_start);
        if (tag_end == std::string::npos) {
            output.append(html, index, std::string::npos);
            break;
        }

        output.append(html, index, tag_start - index);
        const std::string tag = html.substr(tag_start, tag_end - tag_start + 1);
        index = tag_end + 1;

        // The value's offset is what is kept, not the value alone: the same text
        // may appear again in the alt text, where it must not be substituted.
        const std::size_t source_start = tag.find("src=\"");
        if (source_start == std::string::npos) {
            output += tag;
            continue;
        }

        const std::size_t value_start = source_start + 5;
        const std::size_t value_end   = tag.find('"', value_start);
        if (value_end == std::string::npos) {
            output += tag;
            continue;
        }

        const std::string source = tag.substr(value_start, value_end - value_start);

        // Ahead of the root-absolute test below: a remote video still wants to
        // be a <video>, where a remote image is left exactly as written.
        if (!source.empty() && is_video(source)) {
            output += render_video(tag, source, static_dir);
            continue;
        }

        // Only a root-absolute path names a file in static/. A remote URL, or a
        // relative one, is the author's business.
        if (source.empty() || source.front() != '/') {
            output += tag;
            continue;
        }

        // Each variant is the URL to emit and the class that reveals it; a lone
        // entry with no class is the ordinary single-file case.
        std::vector<std::pair<std::string, std::string>> variants;
        if (std::filesystem::exists(static_dir / source.substr(1))) {
            // A drawing named *_auto keeps the colours its exporter baked in and is
            // flipped wholesale on the dark palette by a CSS filter, so no dark
            // value has to be chosen for it — and none can be. The class is the
            // whole mechanism; see "Automatic dark mode" in CLAUDE.md. Only the
            // exact-match branch reads it: a _light/_dark pair is the manual route,
            // and asking for both at once names nothing coherent.
            variants.push_back({source, has_name_suffix(source, "_auto") ? "auto-dark" : ""});
        } else {
            const std::string light = add_suffix(source, "_light");
            const std::string dark  = add_suffix(source, "_dark");

            if (!std::filesystem::exists(static_dir / light.substr(1)) ||
                !std::filesystem::exists(static_dir / dark.substr(1))) {
                std::cerr << "warning: no image at " << source << ", and no _light/_dark pair either\n";
                output += tag;
                continue;
            }

            variants.push_back({light, "light-only"});
            variants.push_back({dark, "dark-only"});
        }

        // A pair is emitted adjacent with no whitespace between the two: they sit
        // in the text flow, where a space would be a visible gap once one shows.
        for (const auto& [url, class_name] : variants) {
            const std::filesystem::path path = static_dir / url.substr(1);

            if (path.extension() == ".svg") {
                const std::string inlined = inline_svg(path, tag, class_name);
                if (!inlined.empty()) {
                    output += inlined;
                    continue;
                }
                std::cerr << "warning: " << path << " is not usable as inline SVG; left as <img>\n";
            }

            std::string variant_tag = tag;
            variant_tag.replace(value_start, value_end - value_start, url);
            output += add_class(variant_tag, class_name);
        }
    }

    return output;
}

// ---------------------------------------------------------------------------
// Render one page body: frontmatter substitution, math protection, markdown.
// Sets has_math so the page can pull in the KaTeX assets only when it needs them.
// ---------------------------------------------------------------------------

static std::string render_body(const std::string& raw_markdown, const toml::table& frontmatter,
                               bool& has_math) {
    std::string markdown = raw_markdown;
    substitute_frontmatter(markdown, frontmatter);

    // Code first: the languages use $ and ^[ as syntax, so a snippet has to be
    // inert before the math and sidenote passes go looking for their own markers.
    std::vector<CodeSpan> code;
    markdown = extract_code(markdown, code);

    std::vector<MathSpan> spans;
    markdown = extract_math(markdown, spans);
    markdown = expand_sidenotes(markdown);

    std::string html = markdown_to_html(markdown);
    html = resolve_images(html);
    restore_math(html, spans);
    // Last, so nothing downstream reads the highlighted markup as its own.
    restore_code(html, code);

    has_math = !spans.empty();
    return html;
}

// ---------------------------------------------------------------------------
// Tag the nav link pointing at the page being rendered, so the active entry can
// be styled without any client-side script. Matching includes the closing quote
// so that href="/" does not also match href="/about.html".
//
// The class is merged into whatever class the link already carries rather than
// appended as a second attribute: a tag with two class attributes keeps only the
// first, so emitting one would silently drop either is-current or the class the
// template set (the name link in the nav carries one).
// ---------------------------------------------------------------------------

static std::string mark_current_link(const std::string& html, const std::string& url) {
    const std::string href = "href=\"" + url + "\"";
    const std::size_t position = html.find(href);
    if (position == std::string::npos)
        return html; // page has no nav entry, e.g. an article

    // Bound the search to this one tag, so a class on a later link is not the
    // one that gets extended.
    const std::size_t tag_start = html.rfind('<', position);
    const std::size_t tag_end   = html.find('>', position);
    const std::size_t existing  = html.rfind("class=\"", tag_end);

    if (existing != std::string::npos && existing > tag_start)
        return html.substr(0, existing + 7) + "is-current " + html.substr(existing + 7);

    const std::size_t insert_at = position + href.size();
    return html.substr(0, insert_at) + " class=\"is-current\"" + html.substr(insert_at);
}

// ---------------------------------------------------------------------------
// Turn a "Month D, YYYY" date into a sortable YYYYMMDD integer.
// Empty or unrecognized dates return 0 so they sort to the bottom.
// ---------------------------------------------------------------------------

static int date_sort_key(const std::string& date) {
    static const char* const month_names[12] = {
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December"};

    // Commas are the only punctuation in the expected format; drop them so the
    // stream sees three plain tokens.
    std::string cleaned = date;
    std::replace(cleaned.begin(), cleaned.end(), ',', ' ');

    std::istringstream stream(cleaned);
    std::string month_name;
    int day = 0, year = 0;
    if (!(stream >> month_name >> day >> year))
        return 0;

    for (int index = 0; index < 12; ++index) {
        if (month_name == month_names[index])
            return year * 10000 + (index + 1) * 100 + day;
    }
    return 0;
}

// Page metadata
struct Page {
    std::filesystem::path source; // Source .md file
    std::filesystem::path output; // where output .html file goes
    std::string url; // the path relative to the output _site directory
    // Frontmatter properties
    std::string title;
    std::string date;
    std::string description; // one-line summary, shown under the title in list pages
    std::string template_name;
    std::string item_template_name;
    std::string list_dir; // for list pages: content subdirectory to list, empty means siblings
    bool show_title = false; // render the title as an <h1> at the top of the page
    bool draft = false; // unfinished: committed to the repo, but not published
    std::string markdown;
    toml::table frontmatter;
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Drafts are left out unless asked for, so the bare ./ssg the deploy
    // workflow runs cannot publish one no matter what is in the repo.
    bool include_drafts = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--drafts") {
            include_drafts = true;
        } else {
            std::cerr << "usage: ssg [--drafts]\n";
            return 1;
        }
    }

    const std::filesystem::path content_dir   = "content";
    const std::filesystem::path output_dir    = "_site";
    const std::filesystem::path static_dir    = "static";
    const std::filesystem::path templates_dir = "templates";

    // Both scripts are deferred, which also guarantees katex.min.js runs first.
    const std::string math_assets =
        "<link rel=\"stylesheet\" href=\"/katex/katex.min.css\">\n"
        "  <script src=\"/katex/katex.min.js\" defer></script>\n"
        "  <script src=\"/js/math.js\" defer></script>";

    const std::string head   = read_file(templates_dir / "partials" / "head.html");
    const std::string header = read_file(templates_dir / "partials" / "header.html");
    const std::string footer = read_file(templates_dir / "partials" / "footer.html");

    // --- First pass: collect all pages ---
    std::vector<Page> pages;
    int skipped_drafts = 0;

    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(content_dir)) {
        // We only care about .md files in the content directory
        if (!entry.is_regular_file() || entry.path().extension() != ".md") {
            continue;
        }

        std::string file_content     = read_file(entry.path());
        toml::table frontmatter      = parse_frontmatter(file_content); // Updates file_content in place to remove the frontmatter section
        const std::string& markdown  = std::move(file_content);

        const std::filesystem::path relative_path = std::filesystem::relative(entry.path(), content_dir);
        const std::filesystem::path output_path   = output_dir / relative_path.parent_path() / (relative_path.stem().string() + ".html");

        // URL is the path relative to _site, prefixed with /
        std::string url = "/" + (relative_path.parent_path() / (relative_path.stem().string() + ".html")).generic_string();
        // index.html -> clean URL (directory root)
        if (relative_path.stem() == "index")
            url = "/" + relative_path.parent_path().generic_string();
        if (url == "/.")
            url = "/";

        // Dropping a draft here rather than at render time is what keeps it out
        // of list pages too: the second pass only ever sees published pages.
        if (frontmatter["draft"].value_or(false) && !include_drafts) {
            // A previous --drafts run leaves the rendered draft behind, and the
            // generator never clears _site, so remove it. Otherwise a local
            // preview would keep serving a page this build deliberately omitted.
            std::filesystem::remove(output_path);
            ++skipped_drafts;
            continue;
        }

        Page page;
        page.source        = entry.path();
        page.output        = output_path;
        page.url           = url;
        page.title         = frontmatter["title"].value_or(entry.path().stem().string());
        page.date               = frontmatter["date"].value_or(std::string(""));
        page.description        = frontmatter["description"].value_or(std::string(""));
        page.template_name      = frontmatter["template"].value_or(std::string("default"));
        page.item_template_name = frontmatter["item_template"].value_or(std::string("list-item"));
        page.list_dir           = frontmatter["list_dir"].value_or(std::string(""));
        page.show_title         = frontmatter["show_title"].value_or(false);
        page.draft              = frontmatter["draft"].value_or(false);
        page.markdown      = std::move(markdown);
        page.frontmatter   = std::move(frontmatter);
        pages.push_back(std::move(page));
    }

    // --- Second pass: render pages ---
    for (const Page& page : pages) {
        std::string content_html;
        bool page_has_math = false;

        if (page.template_name == "list") {
            // List the pages in list_dir, or this page's own directory when unset.
            const std::filesystem::path listed_dir =
                page.list_dir.empty() ? page.output.parent_path() : output_dir / page.list_dir;

            // Collect first so the entries can be ordered before rendering.
            std::vector<const Page*> listed_pages;
            for (const Page& other_page : pages) {
                if (other_page.output == page.output)
                    continue;
                if (other_page.output.parent_path() != listed_dir)
                    continue;
                listed_pages.push_back(&other_page);
            }

            // Newest first; same-date entries fall back to title for a stable order.
            std::sort(listed_pages.begin(), listed_pages.end(),
                      [](const Page* left, const Page* right) {
                          const int left_key  = date_sort_key(left->date);
                          const int right_key = date_sort_key(right->date);
                          if (left_key != right_key)
                              return left_key > right_key;
                          return left->title < right->title;
                      });

            const std::filesystem::path item_tmpl_path = templates_dir / "partials" / (page.item_template_name + ".html");
            const std::string           item_template  = read_file(item_tmpl_path);
            std::string list_html = "<ul class=\"page-list\">\n";
            for (const Page* other_page : listed_pages) {
                std::string item = item_template;
                substitute_frontmatter(item, other_page->frontmatter);
                substitute(item, "url", other_page->url);
                // date and description are optional, and substitute_frontmatter only
                // fills keys that exist. Blank them explicitly so a page missing one
                // does not leak a literal {{date}} / {{description}} into the page.
                substitute(item, "date", other_page->date);
                substitute(item, "description", other_page->description);
                // A draft only reaches a list page under --drafts, so the tag marks
                // which entries a plain ./ssg would have dropped. Blank otherwise,
                // which is also what keeps the placeholder out of the built site.
                substitute(item, "draft_tag",
                           other_page->draft ? " <span class=\"draft-tag\">[DRAFT]</span>" : "");
                list_html += item;
            }
            list_html += "</ul>\n";

            // Any markdown body on the list page itself renders above the list.
            content_html = render_body(page.markdown, page.frontmatter, page_has_math) + list_html;
        } else {
            content_html = render_body(page.markdown, page.frontmatter, page_has_math);
        }

        const std::filesystem::path template_path = templates_dir / (page.template_name + ".html");
        // Opt-in heading, so a page can carry a title for the tab and list entries
        // without repeating it at the top of its own body.
        const std::string page_heading = page.show_title ? "<h1>" + page.title + "</h1>" : "";

        std::string rendered_page = read_file(template_path);
        substitute(rendered_page, "page_heading", page_heading);
        substitute(rendered_page, "head",    head); // before title: the head partial contains {{title}}
        // KaTeX is ~300KB, so it is pulled in only by pages that actually have math.
        // Substituted after head, which is where the placeholder lives.
        substitute(rendered_page, "math_assets", page_has_math ? math_assets : "");
        // In a page template {{title}} only ever reaches the head partial's
        // <title>, so this marks the browser tab of a previewed draft and
        // nothing else on the page.
        substitute(rendered_page, "title",   page.draft ? "[draft] " + page.title : page.title);
        substitute(rendered_page, "header",  mark_current_link(header, page.url));
        substitute(rendered_page, "footer",  footer);
        substitute(rendered_page, "content", content_html);

        write_file(page.output, rendered_page);
        std::cout << "built: " << page.output << (page.draft ? "  (draft)" : "") << "\n";
    }

    if (skipped_drafts > 0)
        std::cout << "skipped " << skipped_drafts << " draft(s); ./ssg --drafts to preview them\n";

    // --- Copy static files ---
    if (std::filesystem::exists(static_dir)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(static_dir)) {
            if (!entry.is_regular_file())
                continue;
            const std::filesystem::path destination =
                output_dir / std::filesystem::relative(entry.path(), static_dir);
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(entry.path(), destination,
                                       std::filesystem::copy_options::overwrite_existing);
        }
    }

    return 0;
}
