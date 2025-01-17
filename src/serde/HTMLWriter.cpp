#include "serde/HTMLWriter.hpp"
#include "ctml.hpp"
#include "spdlog/spdlog.h"
#include "types/Symbols.hpp"
#include "clang/Basic/Specifiers.h"
#include "clang/Format/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"

#include <cmark.h>
#include <filesystem>
#include <fstream>
#include <string>

/// Implementation of to_string() for Clang member variable access specifier
static std::string to_string(const clang::AccessSpecifier& access) {
  switch (access) {
  case clang::AccessSpecifier::AS_public:
    return "public";
  case clang::AccessSpecifier::AS_protected:
    return "protected";
  case clang::AccessSpecifier::AS_private:
    return "private";
  case clang::AccessSpecifier::AS_none:
    return "none";
  default:
    return "unknown";
  }
}

/// Returns a vector of all SymbolIDs in a given database
/// Useful for getting a vector of SymbolIDs that will be sorted
template <typename T> static std::vector<hdoc::types::SymbolID> map2vec(const hdoc::types::Database<T>& db) {
  std::vector<hdoc::types::SymbolID> IDs;
  IDs.reserve(db.entries.size());
  for (const auto& [k, v] : db.entries) {
    IDs.push_back(k);
  }
  return IDs;
}

/// Sort a vector of SymbolIDs alphabetically by the name of the Symbol they point to
/// Note: all members of IDs need to be of type T
template <typename T>
static std::vector<hdoc::types::SymbolID> getSortedIDs(const std::vector<hdoc::types::SymbolID>& IDs,
                                                       const hdoc::types::Database<T>&           db) {
  std::vector<T> symbols = {};
  symbols.reserve(IDs.size());
  for (const auto& id : IDs) {
    symbols.push_back(db.entries.at(id));
  }
  std::sort(symbols.begin(), symbols.end());
  std::vector<hdoc::types::SymbolID> sortedIDs;
  sortedIDs.reserve(IDs.size());
  for (const auto& s : symbols) {
    sortedIDs.push_back(s.ID);
  }
  return sortedIDs;
}

extern uint8_t  ___assets_styles_css[];
extern uint8_t  ___assets_favicon_ico[];
extern uint8_t  ___assets_favicon_32x32_png[];
extern uint8_t  ___assets_favicon_16x16_png[];
extern uint8_t  ___assets_apple_touch_icon_png[];
extern uint8_t  ___assets_search_js[];
extern uint8_t  ___assets_worker_js[];
extern uint64_t ___assets_styles_css_len;
extern uint64_t ___assets_favicon_ico_len;
extern uint64_t ___assets_favicon_32x32_png_len;
extern uint64_t ___assets_favicon_16x16_png_len;
extern uint64_t ___assets_apple_touch_icon_png_len;
extern uint64_t ___assets_search_js_len;
extern uint64_t ___assets_worker_js_len;

hdoc::serde::HTMLWriter::HTMLWriter(const hdoc::types::Index* index, const hdoc::types::Config* cfg)
    : index(index), cfg(cfg) {
  // Create the directory where the HTML files will be placed
  std::error_code ec;
  if (std::filesystem::exists(this->cfg->outputDir) == false) {
    if (std::filesystem::create_directories(this->cfg->outputDir, ec) == false) {
      spdlog::error(
          "Creation of directory {} failed with error message {}", this->cfg->outputDir.string(), ec.message());
    }
  }

  // hdoc bundles assets (favicons, CSS) with the executable to simplify deployment
  // The following code collects the files (converted to char arrays in the build process)
  // and outputs them. The process looks janky but it's simple and it works
  const uint64_t lens[] = {
      ___assets_apple_touch_icon_png_len,
      ___assets_favicon_16x16_png_len,
      ___assets_favicon_32x32_png_len,
      ___assets_favicon_ico_len,
      ___assets_styles_css_len,
      ___assets_search_js_len,
      ___assets_worker_js_len,
  };

  const uint8_t* files[] = {
      ___assets_apple_touch_icon_png,
      ___assets_favicon_16x16_png,
      ___assets_favicon_32x32_png,
      ___assets_favicon_ico,
      ___assets_styles_css,
      ___assets_search_js,
      ___assets_worker_js,
  };

  const std::filesystem::path paths[] = {
      cfg->outputDir / "apple-touch-icon.png",
      cfg->outputDir / "favicon-16x16.png",
      cfg->outputDir / "favicon-32x32.png",
      cfg->outputDir / "favicon.ico",
      cfg->outputDir / "styles.css",
      cfg->outputDir / "search.js",
      cfg->outputDir / "worker.js",
  };

  for (std::size_t i = 0; i < std::size(paths); i++) {
    std::ofstream out(paths[i], std::ios::binary);
    out.write((char*)files[i], lens[i]);
    out.close();
  }
}

/// Create a new HTML page with standard structure
/// Optional sidebar, CSS styling, favicons, footer, etc.
static void printNewPage(const hdoc::types::Config&   cfg,
                         CTML::Node                   main,
                         const std::filesystem::path& path,
                         const std::string&           pageTitle) {
  CTML::Document html;

  // Create the header, which includes Bulma CSS framework
  html.AppendNodeToHead(CTML::Node("meta").SetAttr("charset", "utf-8"));
  html.AppendNodeToHead(
      CTML::Node("meta").SetAttr("name", "viewport").SetAttr("content", "width=device-width, initial-scale=1"));
  html.AppendNodeToHead(CTML::Node("title", pageTitle));

  // Use our custom css which is a modified version of bulma
  html.AppendNodeToHead(CTML::Node("link").SetAttr("rel", "stylesheet").SetAttr("href", "styles.css"));

  // highlight.js stylesheet and scripts
  html.AppendNodeToHead(
      CTML::Node("link")
          .SetAttr("rel", "stylesheet")
          .SetAttr("href", "//cdnjs.cloudflare.com/ajax/libs/highlight.js/9.18.1/styles/foundation.min.css"));
  html.AppendNodeToHead(
      CTML::Node("script").SetAttr("src", "//cdnjs.cloudflare.com/ajax/libs/highlight.js/9.18.1/highlight.min.js"));
  html.AppendNodeToHead(CTML::Node("script", "hljs.initHighlightingOnLoad();"));

  // Favicons
  html.AppendNodeToHead(CTML::Node("link")
                            .SetAttr("rel", "apple-touch-icon")
                            .SetAttr("sizes", "180x180")
                            .SetAttr("href", "apple-touch-icon.png"));
  html.AppendNodeToHead(CTML::Node("link")
                            .SetAttr("rel", "icon")
                            .SetAttr("type", "image/png")
                            .SetAttr("sizes", "32x32")
                            .SetAttr("href", "favicon-32x32.png"));
  html.AppendNodeToHead(CTML::Node("link")
                            .SetAttr("rel", "icon")
                            .SetAttr("type", "image/png")
                            .SetAttr("sizes", "16x16")
                            .SetAttr("href", "favicon-16x16.png"));

  CTML::Node wrapperDiv   = CTML::Node("div#wrapper");
  CTML::Node section      = CTML::Node("section.section");
  CTML::Node containerDiv = CTML::Node("div.container");

  // Create a sidebar with navigation links etc
  auto columnsDiv = CTML::Node("div.columns");
  auto aside      = CTML::Node("aside.column is-one-fifth");
  auto menuUL     = CTML::Node("ul.menu-list");

  aside.AddChild(CTML::Node("a.is-button is-size-1", "hdoc").SetAttr("href", "https://hdoc.io"));
  menuUL.AddChild(CTML::Node("p.is-size-4", cfg.projectName + " " + cfg.projectVersion));
  menuUL.AddChild(CTML::Node("p.menu-label", "Navigation"));
  menuUL.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Home").SetAttr("href", "index.html")));
  menuUL.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Search").SetAttr("href", "search.html")));
  menuUL.AddChild(CTML::Node("p.menu-label", "API Documentation"));
  menuUL.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Functions").SetAttr("href", "functions.html")));
  menuUL.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Records").SetAttr("href", "records.html")));
  menuUL.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Enums").SetAttr("href", "enums.html")));
  menuUL.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Namespaces").SetAttr("href", "namespaces.html")));
  // Add paths to markdown pages converted to HTML, if any were provided
  if (cfg.mdPaths.size() > 0) {
    menuUL.AddChild(CTML::Node("p.menu-label", "Pages"));
    for (const auto& f : cfg.mdPaths) {
      std::string path = "doc" + f.filename().replace_extension("html").string();
      std::string name = f.filename().stem().string();
      menuUL.AddChild(CTML::Node("li").AddChild(CTML::Node("a", name).SetAttr("href", path)));
    }
  }
  aside.AddChild(menuUL);

  columnsDiv.AddChild(aside);
  columnsDiv.AddChild(main.SetAttr("class", "content column"));
  containerDiv.AddChild(columnsDiv);
  section.AddChild(containerDiv);
  wrapperDiv.AddChild(section);
  html.AppendNodeToBody(wrapperDiv);

  // Create footer with creation date and details
  CTML::Node p1 = CTML::Node("p", "Documentation for " + cfg.projectName + " " + cfg.projectVersion + ".");
  CTML::Node p2 = CTML::Node("p", "Generated by hdoc version " + cfg.hdocVersion + " on " + cfg.timestamp + ".");
  html.AppendNodeToBody(CTML::Node("footer.footer").AddChild(p1).AddChild(p2));

  // Dump to a file
  std::ofstream(path) << html.ToString();
}

/// Return a short string describing a symbol for its entry in the overview list
static std::string getSymbolBlurb(const hdoc::types::Symbol& s) {
  if (s.briefComment != "") {
    return " - " + s.briefComment;
  }
  if (s.docComment != "") {
    if (s.docComment.size() > 64) {
      return " - " + s.docComment.substr(0, 63) + "...";
    } else {
      return " - " + s.docComment;
    }
  }
  return "";
}

/// Run clang-format with a custom style over the given string
static std::string clangFormat(const std::string& s, const uint64_t& columnLimit = 50) {
  // Run clang-format over function name to break width to 50 chars
  auto style              = clang::format::getChromiumStyle(clang::format::FormatStyle::LK_Cpp);
  style.ColumnLimit       = columnLimit;
  style.BreakBeforeBraces = clang::format::FormatStyle::BS_Attach;
  auto formattedName =
      clang::tooling::applyAllReplacements(s, clang::format::reformat(style, s, {clang::tooling::Range(0, s.size())}));

  return formattedName.get();
}

/// Print a function to main
static void printFunction(const hdoc::types::FunctionSymbol& f, CTML::Node& main) {
  // Print function return type, name, and parameters as section header
  main.AddChild(CTML::Node("h3#" + f.ID.str())
                    .AddChild(CTML::Node("pre").AddChild(CTML::Node("code.language-cpp", clangFormat(f.proto)))));

  // Print function description
  main.AddChild(CTML::Node("h4", "Description"));
  if (f.briefComment != "") {
    main.AddChild(CTML::Node("p", f.briefComment));
  }
  if (f.docComment != "") {
    main.AddChild(CTML::Node("p", f.docComment));
  }
  main.AddChild(CTML::Node("p", "Declared at: ")
                    .AddChild(CTML::Node("span.is-family-code", f.file + ":" + std::to_string(f.line))));

  // Print function parameters (with type, name, default value, and comment) as a list
  if (f.params.size() > 0) {
    main.AddChild(CTML::Node("h4", "Parameters"));
    CTML::Node dl("dl");

    for (auto param : f.params) {
      CTML::Node dt;
      // Print the access, type, name, and doc comment if it exists
      dt = CTML::Node("dt.is-family-code", param.type + " ").AddChild(CTML::Node("b", param.name));

      if (param.defaultValue != "") {
        dt.AppendText(" = " + param.defaultValue);
      }
      dl.AddChild(dt);
      if (param.docComment != "") {
        dl.AddChild(CTML::Node("dd", param.docComment));
      }
    }
    main.AddChild(dl);
  }

  // Return value description
  if (f.returnTypeDocComment != "") {
    main.AddChild(CTML::Node("h4", "Returns"));
    main.AddChild(CTML::Node("p", f.returnTypeDocComment));
  }
}

/// Print all of the functions that aren't record members in a project
void hdoc::serde::HTMLWriter::printFunctions() const {
  CTML::Node  main("main");
  std::string pageTitle = "Functions";
  main.AddChild(CTML::Node("h1", pageTitle));

  // Print a bullet list of functions
  CTML::Node       ul("ul");
  llvm::ThreadPool pool(cfg->numThreads == 0 ? llvm::hardware_concurrency() : cfg->numThreads);
  for (const auto& id : getSortedIDs(map2vec(this->index->functions), this->index->functions)) {
    const auto& f = this->index->functions.entries.at(id);
    if (f.isRecordMember) {
      continue;
    }
    ul.AddChild(CTML::Node("li")
                    .AddChild(CTML::Node("a.is-family-code", f.name).SetAttr("href", "f" + f.ID.str() + ".html"))
                    .AppendText(getSymbolBlurb(f)));
    CTML::Node page("main");
    pool.async(
        [&](const hdoc::types::FunctionSymbol& func, CTML::Node pg) {
          printFunction(func, pg);
          printNewPage(*this->cfg, pg, this->cfg->outputDir / ("f" + func.ID.str() + ".html"), func.name);
        },
        f,
        page);
  }
  pool.wait();
  main.AddChild(CTML::Node("h2", "Overview"));
  main.AddChild(ul);
  printNewPage(*this->cfg, main, this->cfg->outputDir / "functions.html", pageTitle);
}

static std::vector<hdoc::types::RecordSymbol::BaseRecord> getInheritedSymbols(const hdoc::types::Index*        index,
                                                                              const hdoc::types::RecordSymbol& root) {
  std::vector<hdoc::types::RecordSymbol::BaseRecord> vec   = {};
  std::stack<hdoc::types::RecordSymbol::BaseRecord>  stack = {};
  for (const auto& base : root.baseRecords) {
    stack.push(base);
  }

  // Do a depth-first traversal of the parents of root
  while (!stack.empty()) {
    const hdoc::types::RecordSymbol::BaseRecord record = stack.top();
    stack.pop();

    // Quit if the base record is in std namespace
    if (index->records.contains(record.id) == false) {
      continue;
    }

    // Records inherited privately are ignored and their children are not traversed
    // This is suboptimal since an immediate privately inherited parent of root might have some important members
    // we'd like to document; for now I'm not implementing that since that edge case would balloon code complexity
    if (record.access == clang::AS_private) {
      continue;
    }

    vec.push_back(record);

    // Add children to stack for traversing
    const auto& c = index->records.entries.at(record.id);
    for (const auto& baseRecord : c.baseRecords) {
      stack.push(baseRecord);
    }
  }
  return vec;
}

static void printMemberVariables(const hdoc::types::RecordSymbol& c, CTML::Node& main, const bool& isInherited) {
  CTML::Node dl("dl");
  uint64_t   numVars = 0;

  for (const hdoc::types::MemberVariable& var : c.vars) {
    if (isInherited == true && var.access == clang::AS_private) {
      continue;
    }

    std::string type = to_string(var.access);
    type += var.isStatic ? " static " : " ";
    type += var.type + " ";

    CTML::Node dt;
    // Print the access, type, name, and doc comment if it exists
    if (isInherited == false) {
      dt = CTML::Node("dt.is-family-code", type).AddChild(CTML::Node("b", var.name)).SetAttr("id", "var_" + var.name);
    }
    // Inherited variables get a bullet point and link to the description in the parent record
    else {
      dt           = CTML::Node("dt.is-family-code");
      const auto a = CTML::Node("a", type)
                         .SetAttr("href", "r" + c.ID.str() + ".html#var_" + var.name)
                         .AddChild(CTML::Node("b", var.name));
      dt.AddChild(a);
    }
    if (var.defaultValue != "") {
      dt.AppendText(" = " + var.defaultValue);
    }

    dl.AddChild(dt);

    if (isInherited == false && var.docComment != "") {
      dl.AddChild(CTML::Node("dd", var.docComment));
    }

    numVars += 1;
  }

  if (numVars > 0) {
    if (isInherited) {
      main.AddChild(CTML::Node("p", "Inherited from ")
                        .AddChild(CTML::Node("a", c.name).SetAttr("href", "r" + c.ID.str() + ".html"))
                        .AppendText(":"));
    }
    main.AddChild(dl);
  }
}

/// Print a list of inherited methods for the given record, truncating the method declaration
static void
printInheritedMethods(const hdoc::types::Index* index, const hdoc::types::RecordSymbol& c, CTML::Node& main) {
  auto ul = CTML::Node("ul");

  for (const auto& methodID : getSortedIDs(c.methodIDs, index->functions)) {
    const auto& f = index->functions.entries.at(methodID);
    // Skip private functions and ctors/dtors that aren't inherited
    if (f.access == clang::AS_private || f.isCtorOrDtor) {
      continue;
    }

    const auto li = CTML::Node("li.is-family-code")
                        .AddChild(CTML::Node("a", to_string(f.access) + " ")
                                      .SetAttr("href", "r" + c.ID.str() + ".html#" + f.ID.str())
                                      .AddChild(CTML::Node("b", f.name)));
    ul.AddChild(li);
  }

  if (c.methodIDs.size() > 0) {
    main.AddChild(CTML::Node("p", "Inherited from ")
                      .AddChild(CTML::Node("a", c.name).SetAttr("href", "r" + c.ID.str() + ".html"))
                      .AppendText(":"));
    main.AddChild(ul);
  }
}

/// Print a record to main
void hdoc::serde::HTMLWriter::printRecord(const hdoc::types::RecordSymbol& c) const {
  CTML::Node main("main");

  const std::string pageTitle = c.type + " " + c.name;
  main.AddChild(CTML::Node("h1", pageTitle));

  // Full declaration
  main.AddChild(CTML::Node("h2", "Declaration"));
  main.AddChild(CTML::Node("pre").AddChild(
      CTML::Node("code.language-cpp", clangFormat(c.proto, 70) + " { /* full declaration omitted */ };")));

  // Description
  main.AddChild(CTML::Node("h2", "Description"));
  if (c.briefComment != "") {
    main.AddChild(CTML::Node("p", c.briefComment));
  }
  if (c.docComment != "") {
    main.AddChild(CTML::Node("p", c.docComment));
  }
  main.AddChild(CTML::Node("p", "Declared at: ")
                    .AddChild(CTML::Node("span.is-family-code", c.file + ":" + std::to_string(c.line))));

  // Base records
  uint64_t count = 0;
  if (c.baseRecords.size() > 0) {
    auto baseP = CTML::Node("p", "Inherits from: ");
    for (const auto& baseRecord : c.baseRecords) {
      if (count > 0) {
        baseP.AppendText(", ");
      }
      // Check if type is a string, indicating it's a std record that isn't in the DB
      if (this->index->records.contains(baseRecord.id) == false) {
        baseP.AppendText(baseRecord.name);
      } else {
        const auto& p = this->index->records.entries.at(baseRecord.id);
        baseP.AddChild(CTML::Node("a", p.name).SetAttr("href", "r" + p.ID.str() + ".html"));
      }
      count++;
    }
    main.AddChild(baseP);
  }

  // Print regular member variables
  bool hasMemberVariableHeading = false;
  if (c.vars.size() > 0) {
    main.AddChild(CTML::Node("h2", "Member Variables"));
    hasMemberVariableHeading = true;
    printMemberVariables(c, main, false);
  }

  // Print inherited member variables
  const auto inheritedRecords = getInheritedSymbols(this->index, c);
  for (const auto& base : inheritedRecords) {
    const auto& ic = this->index->records.entries.at(base.id);
    if (hasMemberVariableHeading == false && ic.vars.size() > 0) {
      main.AddChild(CTML::Node("h2", "Member Variables"));
      hasMemberVariableHeading = true;
    }
    printMemberVariables(ic, main, true);
  }

  // Method overview in list form
  const auto& sortedMethodIDs          = getSortedIDs(c.methodIDs, this->index->functions);
  bool        hasMethodOverviewHeading = false;
  if (sortedMethodIDs.size() > 0) {
    main.AddChild(CTML::Node("h2", "Method Overview"));
    hasMethodOverviewHeading = true;
    CTML::Node ul("ul");
    for (auto methodID : sortedMethodIDs) {
      const hdoc::types::FunctionSymbol m = this->index->functions.entries.at(methodID);

      // Divide up the full function declaration so its name can be bold in the HTML
      const uint64_t    nameLen  = m.name.size();
      const std::string preName  = to_string(m.access) + " " + m.proto.substr(0, m.nameStart) + " ";
      const std::string postName = m.proto.substr(m.nameStart + nameLen, m.proto.size() - m.nameStart - nameLen);

      const auto li = CTML::Node("li.is-family-code")
                          .AddChild(CTML::Node("a", preName)
                                        .SetAttr("href", "#" + m.ID.str())
                                        .AddChild(CTML::Node("b", m.name))
                                        .AppendText(postName));
      ul.AddChild(li);
    }
    main.AddChild(ul);
  }

  // Add inherited methods to the list
  for (const auto& base : inheritedRecords) {
    const auto& ic = this->index->records.entries.at(base.id);
    if (hasMethodOverviewHeading == false && c.methodIDs.size() > 0) {
      main.AddChild(CTML::Node("h2", "Method Overview"));
      hasMethodOverviewHeading = true;
    }
    printInheritedMethods(this->index, ic, main);
  }

  // List of methods with full information
  if (sortedMethodIDs.size() > 0) {
    main.AddChild(CTML::Node("h2", "Methods"));
    for (const auto& methodID : sortedMethodIDs) {
      // TODO: get to the bottom of what's causing empty method decls to appear in Writer.hpp
      // For now this hack just avoids printing them, but this shouldn't be necessary
      if (index->functions.contains(methodID) == false) {
        continue;
      }
      printFunction(this->index->functions.entries.at(methodID), main);
    }
  }

  printNewPage(*this->cfg, main, this->cfg->outputDir / ("r" + c.ID.str() + ".html"), pageTitle);
}

/// Print all of the records in a project
void hdoc::serde::HTMLWriter::printRecords() const {
  CTML::Node        main("main");
  const std::string pageTitle = "Records";
  main.AddChild(CTML::Node("h1", pageTitle));

  // List of all the records defined, with links to the individual record HTML
  CTML::Node       ul("ul");
  llvm::ThreadPool pool(cfg->numThreads == 0 ? llvm::hardware_concurrency() : cfg->numThreads);
  for (const auto& id : getSortedIDs(map2vec(this->index->records), this->index->records)) {
    const auto& c = this->index->records.entries.at(id);
    ul.AddChild(
        CTML::Node("li")
            .AddChild(CTML::Node("a.is-family-code", c.type + " " + c.name).SetAttr("href", "r" + c.ID.str() + ".html"))
            .AppendText(getSymbolBlurb(c)));
    pool.async([&](const hdoc::types::RecordSymbol& cls) { printRecord(cls); }, c);
  }
  pool.wait();
  main.AddChild(CTML::Node("h2", "Overview"));
  if (this->index->records.entries.size() == 0) {
    main.AddChild(CTML::Node("p", "No records were declared in this project."));
  } else {
    main.AddChild(ul);
  }
  printNewPage(*this->cfg, main, this->cfg->outputDir / "records.html", pageTitle);
}

/// Recursively print an single namespace and all of its children
/// Should be tail-call optimized
static CTML::Node printNamespace(const hdoc::types::NamespaceSymbol& ns, const hdoc::types::Index& index) {
  // Base case: stop recursion when namespace has no further children
  // and return an empty node, which will not be appended since we have a custom version of CTML
  if (ns.records.size() == 0 && ns.enums.size() == 0 && ns.namespaces.size() == 0) {
    return CTML::Node("");
  }

  auto node  = CTML::Node("li.is-family-code", ns.name);
  auto subUL = CTML::Node("ul");

  const std::vector<hdoc::types::SymbolID> childNamespaces = getSortedIDs(ns.namespaces, index.namespaces);
  const std::vector<hdoc::types::SymbolID> childRecords    = getSortedIDs(ns.records, index.records);
  const std::vector<hdoc::types::SymbolID> childEnums      = getSortedIDs(ns.enums, index.enums);

  for (const auto& childID : childNamespaces) {
    auto childNode = printNamespace(index.namespaces.entries.at(childID), index);
    subUL.AddChild(childNode);
  }
  for (const auto& childID : childRecords) {
    const hdoc::types::RecordSymbol s = index.records.entries.at(childID);
    subUL.AddChild(CTML::Node("li.is-family-code")
                       .AddChild(CTML::Node("a", s.type + " " + s.name).SetAttr("href", "r" + s.ID.str() + ".html")));
  }
  for (const auto& childID : childEnums) {
    const hdoc::types::EnumSymbol s = index.enums.entries.at(childID);
    subUL.AddChild(CTML::Node("li.is-family-code")
                       .AddChild(CTML::Node("a", s.type + " " + s.name).SetAttr("href", "e" + s.ID.str() + ".html")));
  }
  return node.AddChild(subUL);
}

/// Print all of the namespaces in a project in a nice tree-view
void hdoc::serde::HTMLWriter::printNamespaces() const {
  CTML::Node        main("main");
  const std::string pageTitle = "Namespaces";
  main.AddChild(CTML::Node("h1", pageTitle));

  CTML::Node namespaceTree("ul");

  for (const auto& id : getSortedIDs(map2vec(this->index->namespaces), this->index->namespaces)) {
    const auto& ns = this->index->namespaces.entries.at(id);
    // Only recurse root namespaces (that have no parents)
    if (ns.parentNamespaceID.raw() != 0) {
      continue;
    }
    namespaceTree.AddChild(printNamespace(ns, *this->index));
  }
  if (this->index->namespaces.entries.size() == 0) {
    main.AddChild(CTML::Node("p", "No namespaces were declared in this project."));
  } else {
    main.AddChild(namespaceTree);
  }
  printNewPage(*this->cfg, main, this->cfg->outputDir / "namespaces.html", pageTitle);
}

/// Print an enum to main
void hdoc::serde::HTMLWriter::printEnum(const hdoc::types::EnumSymbol& e) const {
  CTML::Node        main("main");
  const std::string pageTitle = e.type + " " + e.name;
  main.AddChild(CTML::Node("h1", pageTitle));

  // Description
  main.AddChild(CTML::Node("h2", "Description"));
  if (e.briefComment != "") {
    main.AddChild(CTML::Node("p", e.briefComment));
  }
  if (e.docComment != "") {
    main.AddChild(CTML::Node("p", e.docComment));
  }
  main.AddChild(CTML::Node("p", "Declared at: ")
                    .AddChild(CTML::Node("span.is-family-code", e.file + ":" + std::to_string(e.line))));

  // Enum members in table format
  main.AddChild(CTML::Node("h2", "Enumerators"));
  if (e.members.size() > 0) {
    // Table and table header nodes
    CTML::Node table("table.table is-narrow is-hoverable");
    CTML::Node table_header_row("tr");
    table_header_row.AddChild(CTML::Node("th", "Name"));
    table_header_row.AddChild(CTML::Node("th", "Value"));
    table_header_row.AddChild(CTML::Node("th", "Comment"));
    table.AddChild(table_header_row);

    // Table rows: one row per enum member
    for (const auto& member : e.members) {
      CTML::Node table_row("tr");
      table_row.AddChild(CTML::Node("td.is-family-code", member.name));
      table_row.AddChild(CTML::Node("td.is-family-code", std::to_string(member.value)));
      table_row.AddChild(CTML::Node("td", member.docComment));
      table.AddChild(table_row);
    }
    main.AddChild(table);
  }

  printNewPage(*this->cfg, main, this->cfg->outputDir / ("e" + e.ID.str() + ".html"), pageTitle);
}

/// Print all of the enums in a project
void hdoc::serde::HTMLWriter::printEnums() const {
  CTML::Node        main("main");
  const std::string pageTitle = "Enums";
  main.AddChild(CTML::Node("h1", pageTitle));

  CTML::Node       ul("ul");
  llvm::ThreadPool pool(cfg->numThreads == 0 ? llvm::hardware_concurrency() : cfg->numThreads);
  for (const auto& id : getSortedIDs(map2vec(this->index->enums), this->index->enums)) {
    const auto& e = this->index->enums.entries.at(id);
    ul.AddChild(
        CTML::Node("li")
            .AddChild(CTML::Node("a.is-family-code", e.type + " " + e.name).SetAttr("href", "e" + e.ID.str() + ".html"))
            .AppendText(getSymbolBlurb(e)));
    pool.async([&](const hdoc::types::EnumSymbol& en) { printEnum(en); }, e);
  }
  pool.wait();
  main.AddChild(CTML::Node("h2", "Overview"));
  if (this->index->enums.entries.size() == 0) {
    main.AddChild(CTML::Node("p", "No enums were declared in this project."));
  } else {
    main.AddChild(ul);
  }
  printNewPage(*this->cfg, main, this->cfg->outputDir / "enums.html", pageTitle);
}

void hdoc::serde::HTMLWriter::printSearchPage() const {
  CTML::Node main("main");

  main.AddChild(CTML::Node("h1", "Search"));
  main.AddChild(CTML::Node("noscript").AddChild(CTML::Node("p", "Search requires Javascript to be enabled.")));
  const auto input = CTML::Node("input.input is-primary#search")
                         .SetAttr("type", "search")
                         .SetAttr("autocomplete", "off")
                         .SetAttr("onkeyup", "updateSearchResults()")
                         .SetAttr("style", "display: none");
  main.AddChild(input);
  main.AddChild(CTML::Node("p#info", "Loading index of all symbols. This may take time for large codebases."));
  main.AddChild(CTML::Node("div.list is-hoverable#results").SetAttr("style", "display: none"));
  main.AddChild(
      CTML::Node("script").SetAttr("src", "https://cdn.jsdelivr.net/npm/minisearch@2.4.1/dist/umd/index.min.js"));
  main.AddChild(CTML::Node("script").SetAttr("src", "search.js"));
  printNewPage(*this->cfg, main, this->cfg->outputDir / "search.html", "Search");

  std::error_code      ec;
  llvm::raw_fd_ostream jsonPath((cfg->outputDir / "index.json").string(), ec);
  llvm::json::OStream  json(jsonPath);

  json.array([&] {
    for (const auto& s : this->index->functions.entries)
      json.object([&] {
        auto& f = s.second;
        json.attribute("sid", f.isRecordMember ? f.parentNamespaceID.str() + ".html#" + f.ID.str() : f.ID.str());
        json.attribute("name", f.name);
        json.attribute("decl", f.proto);
        json.attribute("type", f.isRecordMember ? 0 : 1);
      });

    for (const auto& s : this->index->records.entries) {
      json.object([&] {
        auto& c = s.second;
        json.attribute("sid", c.ID.str());
        json.attribute("name", c.name);
        json.attribute("decl", c.proto);
        if (c.type == "struct") {
          json.attribute("type", 2);
        } else if (c.type == "class") {
          json.attribute("type", 3);
        } else {
          json.attribute("type", 4);
        }
      });
    }

    for (const auto& s : this->index->enums.entries) {
      json.object([&] {
        auto& e = s.second;
        json.attribute("sid", e.ID.str());
        json.attribute("name", e.name);
        json.attribute("decl", e.name);
        json.attribute("type", 5);
      });

      for (const auto& ev : s.second.members) {
        json.object([&] {
          auto& e = s.second;
          json.attribute("sid", e.ID.str());
          json.attribute("name", ev.name);
          json.attribute("decl", e.name + "::" + ev.name);
          json.attribute("type", 6);
        });
      }
    }
  });
}

/// Convert a markdown file to an HTML document using cmark
static CTML::Node convertMarkdown(const std::filesystem::path& mdPath) {
  // Slurp content into a string and convert to HTML
  std::ifstream     ifs(mdPath);
  const std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
  char*             htmlBuf = cmark_markdown_to_html(content.c_str(), content.size(), 0);
  if (!htmlBuf) {
    spdlog::warn("Conversion of {} to HTML failed. Skipping this file.", mdPath.string());
  }

  // Convert HTML back to a string and add it to a node
  std::string html = std::string(htmlBuf);
  free(htmlBuf);
  CTML::Node main("main");
  main.AppendRawHTML(html);

  return main;
}

/// Print the homepage of the documentation
void hdoc::serde::HTMLWriter::printProjectIndex() const {
  CTML::Node        main("main");
  const std::string pageTitle = "Home";

  // If index markdown page was supplied, convert it to markdown and print it
  if (this->cfg->homepage != "") {
    main = convertMarkdown(this->cfg->homepage);
  }
  // Otherwise, create a simple page with links to the documentation
  else {
    main.AddChild(CTML::Node("h1", pageTitle));
    CTML::Node ul("ul");

    ul.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Records").SetAttr("href", "records.html")));
    ul.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Functions").SetAttr("href", "functions.html")));
    ul.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Enums").SetAttr("href", "enums.html")));
    ul.AddChild(CTML::Node("li").AddChild(CTML::Node("a", "Namespaces").SetAttr("href", "namespaces.html")));
    main.AddChild(ul);
  }

  printNewPage(*this->cfg, main, this->cfg->outputDir / "index.html", pageTitle);
}

void hdoc::serde::HTMLWriter::processMarkdownFiles() const {
  for (const auto& f : this->cfg->mdPaths) {
    spdlog::info("Processing markdown file {}", f.string());
    CTML::Node  main      = convertMarkdown(f);
    std::string filename  = "doc" + f.filename().replace_extension("html").string();
    std::string pageTitle = f.filename().stem().string();
    printNewPage(*this->cfg, main, this->cfg->outputDir / filename, pageTitle);
  }
}
