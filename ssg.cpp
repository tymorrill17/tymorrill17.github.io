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

static void substitute(std::string& text, const std::string& key, const std::string& value) {
    const std::string to_replace = "{{" + key + "}}";
    std::size_t position = 0;
    while ((position = text.find(to_replace, position)) != std::string::npos) {
        text.replace(position, to_replace.size(), value);
        position += value.size();
    }
}

static void substitute_frontmatter(std::string& text, const toml::table& frontmatter) {
    for (auto&& [key, value] : frontmatter) {
        if (const auto* string_value = value.as_string())
            substitute(text, std::string(key), string_value->get());
    }
}

// ---------------------------------------------------------------------------
// Use cmark to convert markdown to html
// ---------------------------------------------------------------------------

static std::string markdown_to_html(const std::string& markdown) {
    // allocates char*, must free later
    char* raw_html = cmark_markdown_to_html(markdown.c_str(), markdown.size(), CMARK_OPT_DEFAULT);
    std::string result(raw_html);
    free(raw_html);
    return result;
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
    std::string template_name;
    std::string item_template_name;
    std::string list_dir; // for list pages: content subdirectory to list, empty means siblings
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
        page.template_name      = frontmatter["template"].value_or(std::string("default"));
        page.item_template_name = frontmatter["item_template"].value_or(std::string("list-item"));
        page.list_dir           = frontmatter["list_dir"].value_or(std::string(""));
        page.markdown      = std::move(markdown);
        page.frontmatter   = std::move(frontmatter);
        pages.push_back(std::move(page));
    }

    // --- Second pass: render pages ---
    for (const Page& page : pages) {
        std::string content_html;

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
                list_html += item;
            }
            list_html += "</ul>\n";

            // Any markdown body on the list page itself renders above the list.
            std::string markdown = page.markdown;
            substitute_frontmatter(markdown, page.frontmatter);
            content_html = markdown_to_html(markdown) + list_html;
        } else {
            std::string markdown = page.markdown;
            substitute_frontmatter(markdown, page.frontmatter);
            content_html = markdown_to_html(markdown);
        }

        const std::filesystem::path template_path = templates_dir / (page.template_name + ".html");
        std::string rendered_page = read_file(template_path);
        substitute(rendered_page, "head",    head); // before title: the head partial contains {{title}}
        substitute(rendered_page, "title",   page.title);
        substitute(rendered_page, "header",  header);
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
