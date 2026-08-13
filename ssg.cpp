#include <cmark.h>
#include <toml.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
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
// Render one page body: frontmatter substitution, math protection, markdown.
// Sets has_math so the page can pull in the KaTeX assets only when it needs them.
// ---------------------------------------------------------------------------

static std::string render_body(const std::string& raw_markdown, const toml::table& frontmatter,
                               bool& has_math) {
    std::string markdown = raw_markdown;
    substitute_frontmatter(markdown, frontmatter);

    std::vector<MathSpan> spans;
    markdown = extract_math(markdown, spans);

    std::string html = markdown_to_html(markdown);
    restore_math(html, spans);

    has_math = !spans.empty();
    return html;
}

// ---------------------------------------------------------------------------
// Tag the nav link pointing at the page being rendered, so the active entry can
// be styled without any client-side script. Matching includes the closing quote
// so that href="/" does not also match href="/about.html".
// ---------------------------------------------------------------------------

static std::string mark_current_link(const std::string& html, const std::string& url) {
    const std::string href = "href=\"" + url + "\"";
    const std::size_t position = html.find(href);
    if (position == std::string::npos)
        return html; // page has no nav entry, e.g. an article

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
    std::string markdown;
    toml::table frontmatter;
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
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
        substitute(rendered_page, "title",   page.title);
        substitute(rendered_page, "header",  mark_current_link(header, page.url));
        substitute(rendered_page, "footer",  footer);
        substitute(rendered_page, "content", content_html);

        write_file(page.output, rendered_page);
        std::cout << "built: " << page.output << "\n";
    }

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
