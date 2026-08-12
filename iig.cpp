// SPDX-License-Identifier: MIT
// Copyright (c) 2026 iig-port contributors
// See LICENSE and NOTICE for license terms and third-party notices.

#include <clang-c/Index.h>

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

const char kIIGVersion[] = "DriverKit-440";
const char kIIGBannerVersion[] = "DriverKit-440 portable Jan  9 2026 10:49:41";

struct Options {
	std::string def;
	std::string header;
	std::string impl;
	std::string edits;
	std::string log;
	std::string framework_name;
	std::string deployment_target;
	std::vector<std::string> clang_args;
	bool show_help = false;
	bool show_version = false;
};

struct MethodInfo {
	struct ParamInfo {
		std::string type;
		std::string canonical_type;
		std::string name;
		std::string array_count;
		std::string type_method_name;
		std::string port_disposition;
		std::string object_class_name;
		std::string associated_array_param_name;
		std::string associated_count_param_name;
		std::vector<std::string> annotations;
		unsigned pointer_depth = 0;
		bool is_array_type = false;
		bool is_target = false;
		bool referent_is_const = false;
		bool referent_is_char = false;
		bool class_facts_resolved = false;
		bool object_class_is_serializable = false;
		bool pointee_is_struct = false;
	};

	std::string parent;
	std::string name;
	std::string result_type;
	std::string suffix_attributes;
	std::string type_method_name;
	std::vector<ParamInfo> params;
	std::vector<std::string> annotations;
	unsigned start_line = 0;
	unsigned end_line = 0;
	bool is_static = false;
	bool is_const = false;
	bool is_virtual = false;
	bool is_pure = false;
	bool is_variadic = false;
};

struct ClassInfo {
	std::string name;
	std::string base;
	std::vector<std::string> annotations;
	unsigned start_line = 0;
	unsigned end_line = 0;
	bool from_main_file = false;
	bool is_struct = false;
	bool is_definition = false;
	bool has_iig_impl_metadata = false;
	std::string source_file;
	std::vector<MethodInfo> methods;
};

struct ParseState {
	std::string main_file;
	std::vector<ClassInfo> classes;
	std::vector<MethodInfo> loose_methods;
	std::map<std::string, std::vector<std::string>> includes;
};

std::string declaration_param_type(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param);
std::string method_name_from_type_annotation(const std::string &type);

struct ClassMethodOption {
	const MethodInfo *method = nullptr;
	std::string queue_name;
	uint64_t options = 0;
};

struct ImplMethodOption {
	std::string method_id;
	std::string method_name;
	std::string queue_name;
};

struct ImplClassDescription {
	std::string name;
	std::string super_name;
	std::string availability;
	bool can_remote = false;
	std::vector<std::string> queue_names;
	std::vector<ImplMethodOption> method_options;
	std::vector<ImplMethodOption> meta_method_options;
};

struct ClassRecord {
	std::string name;
	bool has_iig_impl_metadata = false;
};

std::string to_string(CXString s)
{
	const char *c = clang_getCString(s);
	std::string out = c ? c : "";
	clang_disposeString(s);
	return out;
}

void fatal(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fputc('\n', stderr);
	std::exit(1);
}

bool write_file(const std::string &path, const std::string &content)
{
	if (path.empty()) {
		return true;
	}
	std::ofstream out(path);
	if (!out) {
		std::fprintf(stderr, "cannot write %s: %s\n", path.c_str(), std::strerror(errno));
		return false;
	}
	out << content;
	return static_cast<bool>(out);
}

std::vector<std::string> read_lines(const std::string &path)
{
	std::ifstream in(path);
	if (!in) {
		fatal("cannot read %s: %s", path.c_str(), std::strerror(errno));
	}
	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line)) {
		lines.push_back(line);
	}
	return lines;
}

std::string basename_of(const std::string &path)
{
	std::string::size_type slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool has_suffix(const std::string &s, const std::string &suffix)
{
	return s.size() >= suffix.size() &&
	    s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim_copy(const std::string &text)
{
	std::string::size_type first = 0;
	while (first < text.size() &&
	    std::isspace(static_cast<unsigned char>(text[first]))) {
		++first;
	}
	std::string::size_type last = text.size();
	while (last > first &&
	    std::isspace(static_cast<unsigned char>(text[last - 1]))) {
		--last;
	}
	return text.substr(first, last - first);
}

std::string dirname_of(const std::string &path)
{
	std::string::size_type slash = path.find_last_of('/');
	return slash == std::string::npos ? "." : path.substr(0, slash);
}

bool file_exists(const std::string &path)
{
	std::ifstream in(path);
	return static_cast<bool>(in);
}

std::string resolve_include_from_line(const std::string &dir, const std::string &line)
{
	std::string trimmed = trim_copy(line);
	if (trimmed.rfind("#include", 0) != 0) {
		return "";
	}
	std::size_t open = trimmed.find_first_of("<\"");
	if (open == std::string::npos) {
		return "";
	}
	char close_ch = trimmed[open] == '<' ? '>' : '"';
	std::size_t close = trimmed.find(close_ch, open + 1);
	if (close == std::string::npos) {
		return "";
	}
	std::string include = trimmed.substr(open + 1, close - open - 1);
	std::vector<std::string> candidates;
	candidates.push_back(dir + "/" + include);
	candidates.push_back(dir + "/" + basename_of(include));
	for (const std::string &candidate : candidates) {
		if (file_exists(candidate)) {
			return candidate;
		}
	}
	return "";
}

void add_include(ParseState *state, const std::string &source, const std::string &included)
{
	std::vector<std::string> &includes = state->includes[source];
	if (std::find(includes.begin(), includes.end(), included) == includes.end()) {
		includes.push_back(included);
	}
}

std::string json_escape(const std::string &s)
{
	std::string out;
	for (char ch : s) {
		switch (ch) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(ch) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
				out += buf;
			} else {
				out += ch;
			}
		}
	}
	return out;
}

void usage(FILE *out, const char *argv0)
{
	std::fprintf(out,
	    "usage: %s --def <def input file> --header <header output file> --impl <implementation output file> [--edits <sed edits output file>] [--log <log file>] [--framework-name <name>] [--deployment-target <deployment target>] -- <clang args>\n",
	    argv0);
}

bool need_value(int argc, char **argv, int index, const char *option)
{
	if (index + 1 < argc) {
		return false;
	}
	std::fprintf(stderr, "iig: option `%s' requires an argument\n", option);
	usage(stderr, argv[0]);
	std::exit(1);
}

bool valid_deployment_target(const std::string &value)
{
	if (value.empty()) {
		return false;
	}
	unsigned components = 1;
	bool saw_digit = false;
	for (char ch : value) {
		if (std::isdigit(static_cast<unsigned char>(ch))) {
			saw_digit = true;
			continue;
		}
		if (ch == '.') {
			if (!saw_digit || components == 3) {
				return false;
			}
			++components;
			saw_digit = false;
			continue;
		}
		return false;
	}
	return saw_digit;
}

Options parse_args(int argc, char **argv)
{
	Options opt;
	bool clang_tail = false;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (clang_tail) {
			opt.clang_args.push_back(arg);
			continue;
		}
		if (arg == "--") {
			clang_tail = true;
		} else if (arg == "--help") {
			opt.show_help = true;
		} else if (arg == "-h") {
			std::fprintf(stderr, "iig: option requires an argument -- h\n");
			usage(stderr, argv[0]);
			std::exit(1);
		} else if (arg == "--version") {
			opt.show_version = true;
		} else if (arg == "--def") {
			need_value(argc, argv, i, "--def");
			opt.def = argv[++i];
		} else if (arg == "--header") {
			need_value(argc, argv, i, "--header");
			opt.header = argv[++i];
		} else if (arg == "--impl") {
			need_value(argc, argv, i, "--impl");
			opt.impl = argv[++i];
		} else if (arg == "--edits") {
			need_value(argc, argv, i, "--edits");
			opt.edits = argv[++i];
		} else if (arg == "--log") {
			need_value(argc, argv, i, "--log");
			opt.log = argv[++i];
		} else if (arg == "--framework-name") {
			need_value(argc, argv, i, "--framework-name");
			opt.framework_name = argv[++i];
		} else if (arg == "--deployment-target") {
			need_value(argc, argv, i, "--deployment-target");
			opt.deployment_target = argv[++i];
			if (!valid_deployment_target(opt.deployment_target)) {
				usage(stderr, argv[0]);
				std::exit(1);
			}
		} else {
			if (arg.size() > 1 && arg[0] == '-' && arg[1] != '-') {
				std::fprintf(stderr, "iig: invalid option -- %c\n", arg[1]);
				continue;
			}
			if (!arg.empty() && arg[0] != '-') {
				continue;
			}
			std::fprintf(stderr, "iig: unrecognized option `%s'\n", arg.c_str());
			usage(stderr, argv[0]);
			std::exit(1);
		}
	}
	return opt;
}

std::vector<std::string> cursor_annotations(CXCursor cursor)
{
	std::vector<std::string> annotations;
	clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
		if (clang_getCursorKind(child) == CXCursor_AnnotateAttr) {
			auto *out = static_cast<std::vector<std::string> *>(data);
			out->push_back(to_string(clang_getCursorDisplayName(child)));
		}
		return CXChildVisit_Continue;
	}, &annotations);
	return annotations;
}

bool has_annotation(const std::vector<std::string> &annotations, const std::string &name)
{
	return std::find(annotations.begin(), annotations.end(), name) != annotations.end();
}

bool has_any_annotation(const std::vector<std::string> &annotations,
    const std::vector<std::string> &names)
{
	for (const std::string &name : names) {
		if (has_annotation(annotations, name)) {
			return true;
		}
	}
	return false;
}

std::string annotation_value(const std::vector<std::string> &annotations, const std::string &prefix)
{
	for (const std::string &annotation : annotations) {
		if (annotation.rfind(prefix, 0) == 0) {
			return annotation.substr(prefix.size());
		}
	}
	return "";
}

bool compatible_method_signature(const MethodInfo &left, const MethodInfo &right);

bool method_is_localonly(const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	if (has_annotation(method.annotations, "localonly")) {
		return true;
	}
	std::string base = klass.base;
	while (!base.empty()) {
		auto class_it = classes.find(base);
		if (class_it == classes.end()) {
			break;
		}
		const ClassInfo *base_class = class_it->second;
		for (const MethodInfo &base_method : base_class->methods) {
			if (compatible_method_signature(method, base_method) &&
			    has_annotation(base_method.annotations, "localonly")) {
				return true;
			}
		}
		base = base_class->base;
	}
	return false;
}

bool is_rpc_method(const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	if (method_is_localonly(classes, klass, method)) {
		return false;
	}
	if (has_annotation(klass.annotations, "kernel")) {
		return true;
	}
	return has_any_annotation(method.annotations, {
	    "kernel", "local", "localhost", "remote", "invokereply", "reply"
	});
}

bool compatible_result_type(const std::string &left, const std::string &right)
{
	return left == right ||
	    (left == "IOReturn" && right == "kern_return_t") ||
	    (left == "kern_return_t" && right == "IOReturn");
}

bool compatible_method_signature(const MethodInfo &left, const MethodInfo &right)
{
	if (left.name != right.name ||
	    !compatible_result_type(left.result_type, right.result_type) ||
	    left.params.size() != right.params.size()) {
		return false;
	}
	for (std::size_t i = 0; i < left.params.size(); ++i) {
		if (left.params[i].type != right.params[i].type) {
			return false;
		}
	}
	return true;
}

std::map<std::string, const ClassInfo *> class_map(const ParseState &state)
{
	std::map<std::string, const ClassInfo *> classes;
	for (const ClassInfo &klass : state.classes) {
		auto it = classes.find(klass.name);
		if (it == classes.end() ||
		    (!it->second->is_definition && klass.is_definition)) {
			classes[klass.name] = &klass;
		}
	}
	return classes;
}

const MethodInfo *find_root_method(const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method, const ClassInfo **root_class)
{
	const MethodInfo *root = &method;
	const ClassInfo *owner = &klass;
	std::string base = klass.base;
	while (!base.empty()) {
		auto class_it = classes.find(base);
		if (class_it == classes.end()) {
			break;
		}
		const ClassInfo *base_class = class_it->second;
		for (const MethodInfo &base_method : base_class->methods) {
			if (compatible_method_signature(method, base_method)) {
				root = &base_method;
				owner = base_class;
				break;
			}
		}
		base = base_class->base;
	}
	if (root_class) {
		*root_class = owner;
	}
	return root;
}

bool class_or_base_method_has_annotation(const std::map<std::string, const ClassInfo *> &classes,
    const std::string &class_name, const std::string &method_name,
    const std::string &annotation)
{
	auto it = classes.find(class_name);
	if (it == classes.end()) {
		return false;
	}
	const ClassInfo *klass = it->second;
	for (const MethodInfo &method : klass->methods) {
		if (method.name == method_name && has_annotation(method.annotations, annotation)) {
			return true;
		}
	}
	if (!klass->base.empty()) {
		return class_or_base_method_has_annotation(classes, klass->base,
		    method_name, annotation);
	}
	return false;
}

// XNU iokit/Kernel/IOKitDebug.cpp uses this fasthash64 mix for IOKit IDs.
// See NOTICE for the upstream fasthash MIT license text.
uint64_t fasthash64_mix(uint64_t value)
{
	value ^= value >> 23;
	value *= 0x2127599bf4325c37ULL;
	value ^= value >> 47;
	return value;
}

uint64_t iig_hash_bytes(const unsigned char *data, std::size_t len, uint64_t seed)
{
	const uint64_t m = 0x880355f21e6d1965ULL;
	uint64_t h = seed ^ (static_cast<uint64_t>(len) * m);
	std::size_t aligned = len & ~static_cast<std::size_t>(7);

	for (std::size_t off = 0; off < aligned; off += 8) {
		uint64_t v = 0;
		std::memcpy(&v, data + off, sizeof(v));
		h ^= fasthash64_mix(v);
		h *= m;
	}

	data += aligned;
	uint64_t v = 0;
	switch (len & 7) {
	case 7:
		v ^= static_cast<uint64_t>(data[6]) << 48;
		[[fallthrough]];
	case 6:
		v ^= static_cast<uint64_t>(data[5]) << 40;
		[[fallthrough]];
	case 5:
		v ^= static_cast<uint64_t>(data[4]) << 32;
		[[fallthrough]];
	case 4:
		v ^= static_cast<uint64_t>(data[3]) << 24;
		[[fallthrough]];
	case 3:
		v ^= static_cast<uint64_t>(data[2]) << 16;
		[[fallthrough]];
	case 2:
		v ^= static_cast<uint64_t>(data[1]) << 8;
		[[fallthrough]];
	case 1:
		v ^= static_cast<uint64_t>(data[0]);
		h ^= fasthash64_mix(v);
		h *= m;
		break;
	case 0:
		break;
	}

	return fasthash64_mix(h);
}

uint64_t iig_hash_string(const std::string &text, uint64_t seed)
{
	return iig_hash_bytes(reinterpret_cast<const unsigned char *>(text.data()),
	    text.size(), seed);
}

uint64_t method_id_for(const ClassInfo &klass, const MethodInfo &method)
{
	uint64_t id = iig_hash_string(method.name, iig_hash_string(klass.name, 0));
	id &= 0x7fffffffffffffffULL;
	if (has_annotation(klass.annotations, "kernel") ||
	    has_annotation(method.annotations, "kernel")) {
		id |= 0x8000000000000000ULL;
	}
	return id;
}

std::string hex_u64(uint64_t value)
{
	std::ostringstream out;
	out << "0x" << std::hex << std::setfill('0') << std::setw(16)
	    << value << "ULL";
	return out.str();
}

void source_range_lines(CXCursor cursor, unsigned *start, unsigned *end)
{
	CXSourceRange range = clang_getCursorExtent(cursor);
	CXSourceLocation begin = clang_getRangeStart(range);
	CXSourceLocation finish = clang_getRangeEnd(range);
	clang_getSpellingLocation(begin, nullptr, start, nullptr, nullptr);
	clang_getSpellingLocation(finish, nullptr, end, nullptr, nullptr);
}

std::string cursor_file(CXCursor cursor)
{
	CXSourceLocation loc = clang_getCursorLocation(cursor);
	CXFile file = nullptr;
	clang_getSpellingLocation(loc, &file, nullptr, nullptr, nullptr);
	return file ? to_string(clang_getFileName(file)) : "";
}

std::string qualified_type(CXType type)
{
	return to_string(clang_getTypeSpelling(type));
}

std::string cursor_source_text(CXTranslationUnit tu, CXCursor cursor)
{
	CXSourceRange range = clang_getCursorExtent(cursor);
	CXToken *tokens = nullptr;
	unsigned count = 0;
	clang_tokenize(tu, range, &tokens, &count);
	std::string out;
	for (unsigned i = 0; i < count; ++i) {
		if (!out.empty()) {
			out += " ";
		}
		out += to_string(clang_getTokenSpelling(tu, tokens[i]));
	}
	clang_disposeTokens(tu, tokens, count);
	return out;
}

std::string edit_type(CXType type, const std::string &source_text)
{
	CXType canonical = clang_getCanonicalType(type);
	CXTypeKind kind = canonical.kind;
	if (kind == CXType_ConstantArray || kind == CXType_IncompleteArray) {
		CXType elem = clang_getArrayElementType(canonical);
		if (source_text.find("* const") != std::string::npos) {
			CXType pointee = clang_getPointeeType(elem);
			return qualified_type(pointee) + " ** const";
		}
		std::string elem_spelling = qualified_type(elem);
		if (elem_spelling == "char") {
			return "const char *";
		}
		if (source_text.rfind("const ", 0) == 0 ||
		    clang_isConstQualifiedType(type) || clang_isConstQualifiedType(elem)) {
			return "const " + qualified_type(clang_getCanonicalType(elem)) + " *";
		}
		return elem_spelling + " *";
	}
	return qualified_type(type);
}

bool type_referent_is_const(CXType type)
{
	CXType canonical = clang_getCanonicalType(type);
	if (canonical.kind == CXType_Pointer) {
		CXType pointee = clang_getPointeeType(canonical);
		return clang_isConstQualifiedType(pointee);
	}
	if (canonical.kind == CXType_ConstantArray ||
	    canonical.kind == CXType_IncompleteArray) {
		CXType elem = clang_getArrayElementType(canonical);
		return clang_isConstQualifiedType(canonical) ||
		    clang_isConstQualifiedType(elem);
	}
	return false;
}

unsigned type_pointer_depth(CXType type)
{
	unsigned depth = 0;
	CXType current = clang_getCanonicalType(type);
	while (current.kind == CXType_Pointer) {
		++depth;
		current = clang_getCanonicalType(clang_getPointeeType(current));
	}
	return depth;
}

bool type_is_array(CXType type)
{
	CXType canonical = clang_getCanonicalType(type);
	return canonical.kind == CXType_ConstantArray ||
	    canonical.kind == CXType_IncompleteArray;
}

bool type_referent_is_char(CXType type)
{
	CXType canonical = clang_getCanonicalType(type);
	CXType referent = canonical;
	if (canonical.kind == CXType_Pointer) {
		referent = clang_getPointeeType(canonical);
	} else if (canonical.kind == CXType_ConstantArray ||
	    canonical.kind == CXType_IncompleteArray) {
		referent = clang_getArrayElementType(canonical);
	}
	referent = clang_getCanonicalType(referent);
	return referent.kind == CXType_Char_S;
}

std::string availability_attribute_from_source(const std::string &source_text)
{
	std::string compact;
	bool in_string = false;
	bool escaped = false;
	for (char ch : source_text) {
		if (in_string) {
			compact += ch;
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}
		if (ch == '"') {
			in_string = true;
			compact += ch;
		} else if (!std::isspace(static_cast<unsigned char>(ch))) {
			compact += ch;
		}
	}
	std::string prefix = "__attribute__((availability(driverkit,introduced=";
	std::size_t begin = compact.find(prefix);
	if (begin != std::string::npos) {
		std::size_t end = compact.find(")))", begin + prefix.size());
		if (end != std::string::npos) {
			return compact.substr(begin, end + 3 - begin);
		}
	}
	return "";
}

std::string array_bound_from_source(const std::string &source_text)
{
	std::size_t open = source_text.find('[');
	if (open == std::string::npos) {
		return "";
	}
	std::size_t close = source_text.find(']', open + 1);
	if (close == std::string::npos) {
		return "";
	}
	return trim_copy(source_text.substr(open + 1, close - open - 1));
}

std::string array_count_from_source(const std::string &source_text)
{
	std::string count = array_bound_from_source(source_text);
	if (count.empty()) {
		return "";
	}
	for (char ch : count) {
		if (!std::isdigit(static_cast<unsigned char>(ch))) {
			return "";
		}
	}
	return count;
}

std::vector<std::string> identifiers_from_source(const std::string &source_text)
{
	std::vector<std::string> identifiers;
	std::string current;
	for (char ch : source_text) {
		if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
			current += ch;
			continue;
		}
		if (!current.empty()) {
			identifiers.push_back(current);
			current.clear();
		}
	}
	if (!current.empty()) {
		identifiers.push_back(current);
	}
	return identifiers;
}

std::string integer_expression_value(const std::string &expression,
    const std::map<std::string, std::string> &constants)
{
	struct Parser {
		const std::string &text;
		const std::map<std::string, std::string> &constants;
		std::size_t pos = 0;
		bool ok = true;

		void skip_spaces()
		{
			while (pos < text.size() &&
			    std::isspace(static_cast<unsigned char>(text[pos]))) {
				++pos;
			}
		}

		bool parse_number(unsigned long long *out)
		{
			std::size_t start = pos;
			if (pos < text.size() && text[pos] == '0' &&
			    pos + 1 < text.size() &&
			    (text[pos + 1] == 'x' || text[pos + 1] == 'X')) {
				pos += 2;
				while (pos < text.size() &&
				    std::isxdigit(static_cast<unsigned char>(text[pos]))) {
					++pos;
				}
			} else {
				while (pos < text.size() &&
				    std::isdigit(static_cast<unsigned char>(text[pos]))) {
					++pos;
				}
			}
			if (start == pos) {
				return false;
			}
			while (pos < text.size() &&
			    (text[pos] == 'u' || text[pos] == 'U' ||
			    text[pos] == 'l' || text[pos] == 'L')) {
				++pos;
			}
			*out = std::strtoull(text.substr(start, pos - start).c_str(),
			    nullptr, 0);
			return true;
		}

		std::string parse_identifier()
		{
			if (pos >= text.size() ||
			    !(std::isalpha(static_cast<unsigned char>(text[pos])) ||
			    text[pos] == '_')) {
				return "";
			}
			std::size_t start = pos;
			while (pos < text.size() &&
			    (std::isalnum(static_cast<unsigned char>(text[pos])) ||
			    text[pos] == '_')) {
				++pos;
			}
			return text.substr(start, pos - start);
		}

		unsigned long long factor()
		{
			skip_spaces();
			if (pos < text.size() && text[pos] == '(') {
				++pos;
				unsigned long long value = expression();
				skip_spaces();
				if (pos >= text.size() || text[pos] != ')') {
					ok = false;
					return 0;
				}
				++pos;
				return value;
			}
			unsigned long long value = 0;
			if (parse_number(&value)) {
				return value;
			}
			std::string id = parse_identifier();
			if (!id.empty()) {
				auto it = constants.find(id);
				if (it != constants.end()) {
					return std::strtoull(it->second.c_str(), nullptr, 10);
				}
			}
			ok = false;
			return 0;
		}

		unsigned long long term()
		{
			unsigned long long value = factor();
			while (ok) {
				skip_spaces();
				if (pos >= text.size() ||
				    (text[pos] != '*' && text[pos] != '/' && text[pos] != '%')) {
					break;
				}
				char op = text[pos++];
				unsigned long long rhs = factor();
				if ((op == '/' || op == '%') && rhs == 0) {
					ok = false;
					return 0;
				}
				if (op == '*') {
					value *= rhs;
				} else if (op == '/') {
					value /= rhs;
				} else {
					value %= rhs;
				}
			}
			return value;
		}

		unsigned long long additive()
		{
			unsigned long long value = term();
			while (ok) {
				skip_spaces();
				if (pos >= text.size() || (text[pos] != '+' && text[pos] != '-')) {
					break;
				}
				char op = text[pos++];
				unsigned long long rhs = term();
				if (op == '+') {
					value += rhs;
				} else {
					if (rhs > value) {
						ok = false;
						return 0;
					}
					value -= rhs;
				}
			}
			return value;
		}

		unsigned long long shift()
		{
			unsigned long long value = additive();
			while (ok) {
				skip_spaces();
				if (pos + 1 >= text.size() ||
				    !((text[pos] == '<' && text[pos + 1] == '<') ||
				    (text[pos] == '>' && text[pos + 1] == '>'))) {
					break;
				}
				char op = text[pos];
				pos += 2;
				unsigned long long rhs = additive();
				if (rhs >= 64) {
					ok = false;
					return 0;
				}
				if (op == '<') {
					value <<= rhs;
				} else {
					value >>= rhs;
				}
			}
			return value;
		}

		unsigned long long bit_and()
		{
			unsigned long long value = shift();
			while (ok) {
				skip_spaces();
				if (pos >= text.size() || text[pos] != '&') {
					break;
				}
				++pos;
				value &= shift();
			}
			return value;
		}

		unsigned long long bit_xor()
		{
			unsigned long long value = bit_and();
			while (ok) {
				skip_spaces();
				if (pos >= text.size() || text[pos] != '^') {
					break;
				}
				++pos;
				value ^= bit_and();
			}
			return value;
		}

		unsigned long long expression()
		{
			unsigned long long value = bit_xor();
			while (ok) {
				skip_spaces();
				if (pos >= text.size() || text[pos] != '|') {
					break;
				}
				++pos;
				value |= bit_xor();
			}
			return value;
		}
	};
	std::string trimmed = trim_copy(expression);
	std::size_t comment = trimmed.find("//");
	if (comment != std::string::npos) {
		trimmed = trim_copy(trimmed.substr(0, comment));
	}
	Parser parser{trimmed, constants};
	unsigned long long value = parser.expression();
	parser.skip_spaces();
	if (!parser.ok || parser.pos != trimmed.size()) {
		return "";
	}
	return std::to_string(value);
}

std::map<std::string, std::string> integer_constants_from_lines(
    const std::vector<std::string> &lines)
{
	std::map<std::string, std::string> raw_constants;
	std::map<std::string, std::string> constants;
	for (const std::string &line : lines) {
		std::vector<std::string> ids = identifiers_from_source(line);
		if (ids.size() >= 3 && ids[0] == "define") {
			std::size_t define_pos = line.find("define");
			std::size_t name_pos = line.find(ids[1], define_pos);
			if (name_pos != std::string::npos) {
				raw_constants[ids[1]] =
				    trim_copy(line.substr(name_pos + ids[1].size()));
			}
		}
		std::size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string name = trim_copy(line.substr(0, eq));
		std::string value = trim_copy(line.substr(eq + 1));
		if (!value.empty() && value.back() == ',') {
			value.pop_back();
		}
		name = trim_copy(name);
		value = trim_copy(value);
		if (name.empty() || value.empty()) {
			continue;
		}
		raw_constants[name] = value;
	}
	bool changed = true;
	while (changed) {
		changed = false;
		for (const auto &entry : raw_constants) {
			if (constants.find(entry.first) != constants.end()) {
				continue;
			}
			std::string value = integer_expression_value(entry.second, constants);
			if (!value.empty()) {
				constants[entry.first] = value;
				changed = true;
			}
		}
	}
	return constants;
}

std::vector<std::string> declaration_lines_for_file_and_includes(
    const std::string &file, std::set<std::string> *visited)
{
	if (!visited->insert(file).second) {
		return {};
	}
	std::vector<std::string> lines = read_lines(file);
	std::vector<std::string> all;
	std::string dir = dirname_of(file);
	for (const std::string &line : lines) {
		std::string include = resolve_include_from_line(dir, line);
		if (!include.empty()) {
			std::vector<std::string> included_lines =
			    declaration_lines_for_file_and_includes(include, visited);
			all.insert(all.end(), included_lines.begin(), included_lines.end());
		}
		all.push_back(line);
	}
	return all;
}

std::vector<std::string> declaration_lines_for_file_and_includes(
    const std::string &file)
{
	std::set<std::string> visited;
	return declaration_lines_for_file_and_includes(file, &visited);
}

std::map<std::string, std::string> typedef_array_counts_for_file(
    const std::string &file)
{
	std::map<std::string, std::string> counts;
	std::vector<std::string> lines =
	    declaration_lines_for_file_and_includes(file);
	std::map<std::string, std::string> constants =
	    integer_constants_from_lines(lines);
	for (const std::string &line : lines) {
		std::size_t typedef_pos = line.find("typedef");
		std::size_t open = line.find('[', typedef_pos);
		std::size_t close = line.find(']',
		    open == std::string::npos ? 0 : open + 1);
		if (typedef_pos == std::string::npos) {
			continue;
		}
		if (open != std::string::npos && close != std::string::npos) {
			std::string before = line.substr(typedef_pos, open - typedef_pos);
			std::vector<std::string> ids = identifiers_from_source(before);
			if (ids.empty()) {
				continue;
			}
			std::string name = ids.back();
			std::string count = trim_copy(line.substr(open + 1, close - open - 1));
			std::string resolved_count = integer_expression_value(count, constants);
			if (!resolved_count.empty()) {
				count = resolved_count;
			}
			bool numeric = !count.empty();
			for (char ch : count) {
				if (!std::isdigit(static_cast<unsigned char>(ch))) {
					numeric = false;
					break;
				}
			}
			if (numeric) {
				counts[name] = count;
			}
			continue;
		}
		std::vector<std::string> ids = identifiers_from_source(line);
		if (ids.size() < 3) {
			continue;
		}
		std::string name = ids.back();
		std::string base = ids[ids.size() - 2];
		auto base_count = counts.find(base);
		if (base_count != counts.end()) {
			counts[name] = base_count->second;
		}
	}
	return counts;
}

std::string array_constant_count_from_source(const std::string &file,
    const std::string &source_text)
{
	std::string bound = array_bound_from_source(source_text);
	if (bound.empty()) {
		return "";
	}
	std::map<std::string, std::string> constants =
	    integer_constants_from_lines(
	        declaration_lines_for_file_and_includes(file));
	std::string value = integer_expression_value(bound, constants);
	if (!value.empty()) {
		return value;
	}
	return "";
}

std::string typedef_array_count_from_source(const std::string &file,
    const std::string &source_text)
{
	std::map<std::string, std::string> counts =
	    typedef_array_counts_for_file(file);
	for (const std::string &id : identifiers_from_source(source_text)) {
		auto it = counts.find(id);
		if (it != counts.end()) {
			return it->second;
		}
	}
	return "";
}

std::string declaration_source_from_file(const std::string &file,
    unsigned start_line, unsigned end_line)
{
	if (start_line == 0 || end_line == 0) {
		return "";
	}
	std::vector<std::string> lines = read_lines(file);
	if (lines.empty() || start_line > lines.size()) {
		return "";
	}
	unsigned last = std::max(start_line, end_line);
	while (last < lines.size() && lines[last - 1].find(';') == std::string::npos) {
		++last;
	}
	std::ostringstream out;
	for (unsigned line = start_line; line <= last && line <= lines.size(); ++line) {
		out << lines[line - 1] << "\n";
	}
	return out.str();
}

MethodInfo method_from_cursor(CXTranslationUnit tu, CXCursor cursor, const std::string &parent)
{
	MethodInfo method;
	std::string source_text = cursor_source_text(tu, cursor);
	method.parent = parent;
	method.name = to_string(clang_getCursorSpelling(cursor));
	method.result_type = qualified_type(clang_getCursorResultType(cursor));
	method.annotations = cursor_annotations(cursor);
	method.type_method_name = method_name_from_type_annotation(
	    annotation_value(method.annotations, "type="));
	source_range_lines(cursor, &method.start_line, &method.end_line);
	std::string declaration_source =
	    declaration_source_from_file(cursor_file(cursor), method.start_line,
	        method.end_line);
	method.suffix_attributes = availability_attribute_from_source(
	    declaration_source.empty() ? source_text : declaration_source);
	method.is_static = clang_CXXMethod_isStatic(cursor);
	method.is_const = clang_CXXMethod_isConst(cursor);
	method.is_virtual = clang_CXXMethod_isVirtual(cursor);
	method.is_pure = clang_CXXMethod_isPureVirtual(cursor);
	method.is_variadic = clang_Cursor_isVariadic(cursor);

	int nargs = clang_Cursor_getNumArguments(cursor);
	for (int i = 0; i < nargs; ++i) {
		CXCursor arg = clang_Cursor_getArgument(cursor, i);
		std::string param_source = cursor_source_text(tu, arg);
		MethodInfo::ParamInfo param;
		CXType arg_type = clang_getCursorType(arg);
		param.type = edit_type(arg_type, param_source);
		param.canonical_type = edit_type(clang_getCanonicalType(arg_type),
		    param_source);
		param.pointer_depth = type_pointer_depth(arg_type);
		param.is_array_type = type_is_array(arg_type);
		param.referent_is_const = type_referent_is_const(arg_type);
		param.referent_is_char = type_referent_is_char(arg_type);
		param.name = to_string(clang_getCursorSpelling(arg));
		param.array_count = array_count_from_source(param_source);
		if (param.array_count.empty()) {
			param.array_count = array_constant_count_from_source(cursor_file(arg),
			    param_source);
		}
		if (param.array_count.empty()) {
			param.array_count = typedef_array_count_from_source(cursor_file(arg),
			    param_source);
		}
		param.annotations = cursor_annotations(arg);
		param.is_target = has_annotation(param.annotations, "target");
		param.type_method_name = method_name_from_type_annotation(
		    annotation_value(param.annotations, "type="));
		for (const std::string &annotation : param.annotations) {
			if (annotation.rfind("MACH_MSG_TYPE_", 0) == 0) {
				param.port_disposition = annotation;
				break;
			}
		}
		method.params.push_back(std::move(param));
	}
	(void)tu;
	return method;
}

CXChildVisitResult collect_class_children(CXCursor cursor, CXCursor, CXClientData data)
{
	auto *klass = static_cast<ClassInfo *>(data);
	CXCursorKind kind = clang_getCursorKind(cursor);
	if (kind == CXCursor_CXXBaseSpecifier) {
		CXType type = clang_getCursorType(cursor);
		klass->base = qualified_type(type);
		klass->has_iig_impl_metadata = true;
		return CXChildVisit_Continue;
	}
	if (kind == CXCursor_FieldDecl) {
		klass->has_iig_impl_metadata = true;
		return CXChildVisit_Continue;
	}
	if (kind == CXCursor_CXXMethod || kind == CXCursor_Constructor || kind == CXCursor_Destructor) {
		CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
		klass->has_iig_impl_metadata = true;
		klass->methods.push_back(method_from_cursor(tu, cursor, klass->name));
		return CXChildVisit_Continue;
	}
	return CXChildVisit_Continue;
}

CXChildVisitResult collect_tu(CXCursor cursor, CXCursor, CXClientData data)
{
	auto *state = static_cast<ParseState *>(data);
	bool from_main = clang_Location_isFromMainFile(clang_getCursorLocation(cursor));
	std::string file = cursor_file(cursor);
	if (!from_main && !has_suffix(file, ".iig")) {
		return CXChildVisit_Continue;
	}
	CXCursorKind kind = clang_getCursorKind(cursor);
	if (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) {
		std::string name = to_string(clang_getCursorSpelling(cursor));
		if (name.empty()) {
			return CXChildVisit_Recurse;
		}
		ClassInfo klass;
		klass.name = name;
		klass.is_struct = kind == CXCursor_StructDecl;
		klass.is_definition = clang_isCursorDefinition(cursor);
		klass.annotations = cursor_annotations(cursor);
		klass.source_file = cursor_file(cursor);
		source_range_lines(cursor, &klass.start_line, &klass.end_line);
		klass.from_main_file = from_main;
		clang_visitChildren(cursor, collect_class_children, &klass);
		state->classes.push_back(std::move(klass));
		return CXChildVisit_Continue;
	}
	return CXChildVisit_Recurse;
}

void inclusion_visitor(CXFile included_file, CXSourceLocation *stack, unsigned stack_len, CXClientData data)
{
	auto *state = static_cast<ParseState *>(data);
	if (stack_len == 0) {
		return;
	}
	CXFile source_file = nullptr;
	clang_getSpellingLocation(stack[0], &source_file, nullptr, nullptr, nullptr);
	if (!source_file || !included_file) {
		return;
	}
	std::string source = to_string(clang_getFileName(source_file));
	std::string included = to_string(clang_getFileName(included_file));
	if (source.empty() || included.empty()) {
		return;
	}
	if (source == state->main_file || has_suffix(source, ".iig")) {
		add_include(state, source, included);
	}
}

void add_direct_iig_includes(const Options &opt, ParseState *state)
{
	std::vector<std::string> lines = read_lines(opt.def);
	std::string dir = dirname_of(opt.def);
	for (const std::string &line : lines) {
		std::string::size_type include_pos = line.find("#include");
		if (include_pos == std::string::npos) {
			continue;
		}
		std::string::size_type suffix_pos = line.find(".iig");
		if (suffix_pos == std::string::npos) {
			continue;
		}
		std::string::size_type slash = line.rfind('/', suffix_pos);
		std::string::size_type begin = slash == std::string::npos ? include_pos : slash + 1;
		std::string name = line.substr(begin, suffix_pos + 4 - begin);
		add_include(state, state->main_file, dir + "/" + name);
	}
}

void resolve_parameter_facts(ParseState *state);

int parse_iig(const Options &opt, ParseState *state)
{
	for (std::size_t i = 0; i < opt.clang_args.size(); ++i) {
		const std::string &arg = opt.clang_args[i];
		if (arg == "-arch" || arg == "-triple") {
			std::fprintf(stderr, "error: -arch, and -triple arguments are not permitted\n");
			return 1;
		}
		if (arg == "-target") {
			if (i + 1 >= opt.clang_args.size() ||
			    opt.clang_args[i + 1].rfind("unknown-", 0) != 0) {
				std::fprintf(stderr, "error: -target is only permitted with an unknown arch\n");
				return 1;
			}
			++i;
		}
	}

	CXIndex index = clang_createIndex(0, 0);
	std::vector<const char *> args;
	for (const std::string &arg : opt.clang_args) {
		args.push_back(arg.c_str());
	}
	CXTranslationUnit tu = nullptr;
	CXErrorCode err = clang_parseTranslationUnit2(index, opt.def.c_str(),
	    args.data(), static_cast<int>(args.size()), nullptr, 0,
	    CXTranslationUnit_DetailedPreprocessingRecord, &tu);
	if (err != CXError_Success || tu == nullptr) {
		clang_disposeIndex(index);
		std::fprintf(stderr, "clang_createTranslationUnitFromSourceFile error\n");
		return 1;
	}

	unsigned diagnostic_count = clang_getNumDiagnostics(tu);
	bool failed = false;
	for (unsigned i = 0; i < diagnostic_count; ++i) {
		CXDiagnostic diag = clang_getDiagnostic(tu, i);
		CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diag);
		if (severity >= CXDiagnostic_Error) {
			failed = true;
		}
		std::fprintf(stderr, "%s\n", to_string(clang_formatDiagnostic(diag,
		    clang_defaultDiagnosticDisplayOptions())).c_str());
		clang_disposeDiagnostic(diag);
	}
	if (failed) {
		clang_disposeTranslationUnit(tu);
		clang_disposeIndex(index);
		std::fprintf(stderr, "compile failed\n");
		return 1;
	}

	CXFile main = clang_getFile(tu, opt.def.c_str());
	state->main_file = main ? to_string(clang_getFileName(main)) : opt.def;
	clang_visitChildren(clang_getTranslationUnitCursor(tu), collect_tu, state);
	clang_getInclusions(tu, inclusion_visitor, state);
	add_direct_iig_includes(opt, state);
	resolve_parameter_facts(state);

	clang_disposeTranslationUnit(tu);
	clang_disposeIndex(index);
	return 0;
}

std::string rewrite_include_line(const std::string &line)
{
	std::string out = line;
	std::string::size_type pos = out.find(".iig");
	if (pos != std::string::npos) {
		out.replace(pos, 4, ".h");
		out += "  /* .iig include */";
	}
	return out;
}

bool iig_only_block_contains_type_decl(const std::vector<std::string> &lines,
    unsigned first_line)
{
	unsigned depth = 0;
	for (unsigned i = first_line; i <= lines.size(); ++i) {
		std::string trimmed = trim_copy(lines[i - 1]);
		if (trimmed.rfind("#if", 0) == 0) {
			++depth;
			continue;
		}
		if (trimmed.rfind("#endif", 0) == 0) {
			if (depth == 0) {
				return false;
			}
			--depth;
			if (depth == 0) {
				return false;
			}
			continue;
		}
		if (depth == 1 &&
		    (trimmed.rfind("class ", 0) == 0 ||
		    trimmed.rfind("struct ", 0) == 0)) {
			return true;
		}
	}
	return false;
}

unsigned class_header_start_line(const std::vector<std::string> &lines,
    const ClassInfo &klass)
{
	unsigned start = klass.start_line;
	if (start == 0 || start > lines.size()) {
		return start;
	}
	unsigned previous = start - 1;
	while (previous > 0 && trim_copy(lines[previous - 1]).empty()) {
		--previous;
	}
	if (previous > 0 && has_suffix(trim_copy(lines[previous - 1]), "*/")) {
		unsigned comment_start = previous;
		while (comment_start > 1 &&
		    trim_copy(lines[comment_start - 1]).find("/*") == std::string::npos) {
			--comment_start;
		}
		if (trim_copy(lines[comment_start - 1]).find("/*") != std::string::npos) {
			return comment_start;
		}
	}
	return start;
}

bool is_generated_header_class(const ClassInfo &klass)
{
	if (klass.name == "OSMetaClassBase") {
		return false;
	}
	if (has_any_annotation(klass.annotations, {
	    "kernel", "native"
	})) {
		return true;
	}
	if (klass.is_definition && has_any_annotation(klass.annotations, {
	    "serializable", "concrete"
	})) {
		return true;
	}
	if (klass.methods.empty()) {
		return false;
	}
	for (const MethodInfo &method : klass.methods) {
		if (has_any_annotation(method.annotations, {
		    "kernel", "local", "localonly", "remote", "localhost",
		    "invokereply", "reply", "target", "MACH_MSG_TYPE_MAKE_SEND",
		    "MACH_MSG_TYPE_COPY_SEND"
		})) {
			return true;
		}
		if (!method.type_method_name.empty()) {
			return true;
		}
	}
	return !annotation_value(klass.annotations, "extends=").empty();
}

std::string method_params_joined(const MethodInfo &method)
{
	std::ostringstream out;
	for (std::size_t i = 0; i < method.params.size(); ++i) {
		if (i != 0) {
			out << ", ";
		}
		out << method.params[i].type;
		if (!method.params[i].name.empty()) {
			out << " " << method.params[i].name;
		}
	}
	if (method.is_variadic) {
		if (!method.params.empty()) {
			out << ", ";
		}
		out << "...";
	}
	return out.str();
}

bool method_name_has_get_prefix(const std::string &name)
{
	return name.rfind("Get", 0) == 0 || name.rfind("get", 0) == 0;
}

std::string method_name_from_type_annotation(const std::string &type)
{
	std::size_t pos = type.rfind("::");
	if (pos != std::string::npos) {
		return type.substr(pos + 2);
	}
	return type;
}

bool method_has_type_annotation(const MethodInfo &method)
{
	return !method.type_method_name.empty();
}

std::string object_class_name_for_param(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param);
bool param_is_serializable_object_pointer(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param);

const MethodInfo::ParamInfo *method_target_param(const MethodInfo &method)
{
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (param.is_target) {
			return &param;
		}
	}
	return nullptr;
}

bool method_has_osaction_target_param(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method)
{
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (!param.is_target) {
			continue;
		}
		return object_class_name_for_param(classes, param) == "OSAction";
	}
	return false;
}

std::string action_class_name(const ClassInfo &klass, const MethodInfo &method);
std::vector<std::string> action_class_names_for_type_methods(const ClassInfo &klass);
bool class_has_metaclass_base(const ClassInfo &klass);
std::string object_class_name_for_type(
    const std::map<std::string, const ClassInfo *> &classes,
    const std::string &type);
std::string object_class_name_for_param(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param);

std::vector<ClassRecord> class_records(const ParseState &state)
{
	std::vector<ClassRecord> records;
	for (const ClassInfo &klass : state.classes) {
		if (klass.is_struct || klass.name.empty()) {
			continue;
		}
		ClassRecord record{klass.name, klass.has_iig_impl_metadata};
		auto it = std::find_if(records.begin(), records.end(),
		    [&](const ClassRecord &existing) {
			    return existing.name == klass.name;
		    });
		if (it != records.end()) {
			record.has_iig_impl_metadata |= it->has_iig_impl_metadata;
			records.erase(it);
		}
		records.push_back(record);
	}
	for (const ClassInfo &klass : state.classes) {
		if (klass.is_struct || klass.name.empty()) {
			continue;
		}
		for (const std::string &action_class :
		    action_class_names_for_type_methods(klass)) {
			records.push_back(ClassRecord{action_class, false});
		}
	}
	return records;
}

std::string action_class_name(const ClassInfo &klass, const MethodInfo &method)
{
	return "OSAction_" + klass.name + "_" + method.name;
}

bool is_retained_object_out_parameter(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	if (param.pointer_depth < 2) {
		return false;
	}
	return !object_class_name_for_param(classes, param).empty() &&
	    !param_is_serializable_object_pointer(classes, param);
}

std::string parameter_prefix_for_wrapped_declaration(const MethodInfo &method,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	if (method_name_has_get_prefix(method.name) &&
	    is_retained_object_out_parameter(classes, param)) {
		return "__attribute__((os_returns_retained)) ";
	}
	return "";
}

std::string method_declaration(const MethodInfo &method, bool virtual_override)
{
	std::ostringstream out;
	if (virtual_override) {
		out << "virtual ";
	} else if (method.is_static) {
		out << "static ";
	}
	out << method.result_type << " " << method.name << "("
	    << method_params_joined(method) << ")";
	if (method.is_const) {
		out << " const";
	}
	if (!method.suffix_attributes.empty()) {
		out << " " << method.suffix_attributes;
	}
	if (virtual_override) {
		out << " APPLE_KEXT_OVERRIDE";
	}
	out << ";";
	return out.str();
}

void write_macro_line(std::ostringstream &out, const std::string &line, bool cont)
{
	out << line;
	if (cont) {
		out << "\\";
	}
	out << "\n";
}

void write_macro_blank(std::ostringstream &out)
{
	write_macro_line(out, "", true);
}

void write_wrapped_method_decl(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method, const std::string &name, bool as_virtual,
    bool as_static, bool include_supermethod, bool apple_override)
{
	std::string prefix = "    ";
	if (as_virtual) {
		prefix += "virtual ";
	} else if (as_static || method.is_static) {
		prefix += "static ";
	}
	bool include_rpc_argument = has_annotation(method.annotations, "reply");
	out << prefix << (include_rpc_argument ? "kern_return_t" : method.result_type) << "\\\n";
	out << "    " << name << "(\\\n";
	if (include_rpc_argument) {
		out << "        IORPC rpc";
		if (!method.params.empty() || include_supermethod) {
			out << ",";
		}
		out << "\\\n";
	}
	for (std::size_t i = 0; i < method.params.size(); ++i) {
		out << "        "
		    << parameter_prefix_for_wrapped_declaration(method, classes,
		    method.params[i])
		    << declaration_param_type(classes, method.params[i]);
		if (!method.params[i].name.empty()) {
			out << " " << method.params[i].name;
		}
		if (!include_supermethod && !method.is_variadic && i + 1 == method.params.size()) {
			out << ")";
			if (method.is_const) {
				out << " const";
			}
			if (!method.suffix_attributes.empty()) {
				out << " " << method.suffix_attributes;
			}
			if (apple_override) {
				out << " APPLE_KEXT_OVERRIDE";
			}
			out << ";\\\n";
			return;
		}
		if (include_supermethod || method.is_variadic || i + 1 != method.params.size()) {
			out << ",";
		}
		out << "\\\n";
	}
	if (method.is_variadic && as_virtual && !include_supermethod) {
		out << "        ...";
		if (method.is_const) {
			out << ") const";
		} else {
			out << ")";
		}
		if (!method.suffix_attributes.empty()) {
			out << " " << method.suffix_attributes;
		}
		if (apple_override) {
			out << " APPLE_KEXT_OVERRIDE";
		}
		out << ";\\\n";
		return;
	}
	if (include_supermethod) {
		out << "        OSDispatchMethod supermethod = NULL";
	}
	out << ")";
	if (method.is_const) {
		out << " const";
	}
	if (!method.suffix_attributes.empty()) {
		out << " " << method.suffix_attributes;
	}
	if (apple_override) {
		out << " APPLE_KEXT_OVERRIDE";
	}
	out << ";\\\n";
}

std::vector<const MethodInfo *> generated_methods_for_class(const ParseState &state,
    const ClassInfo &klass)
{
	std::vector<const MethodInfo *> methods;
	for (auto it = state.classes.rbegin(); it != state.classes.rend(); ++it) {
		const ClassInfo &extension = *it;
		if (annotation_value(extension.annotations, "extends=") != klass.name) {
			continue;
		}
		for (const MethodInfo &method : extension.methods) {
			methods.push_back(&method);
		}
	}
	for (const MethodInfo &method : klass.methods) {
		methods.push_back(&method);
	}
	return methods;
}

std::vector<const MethodInfo *> rpc_methods_for_class(const ParseState &state,
    const ClassInfo &klass)
{
	std::vector<const MethodInfo *> methods;
	std::map<std::string, const ClassInfo *> classes = class_map(state);
	for (const MethodInfo *method : generated_methods_for_class(state, klass)) {
		if (is_rpc_method(classes, klass, *method)) {
			methods.push_back(method);
		}
	}
	return methods;
}

bool method_parent_extends_class(const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	auto it = classes.find(method.parent);
	return it != classes.end() &&
	    annotation_value(it->second->annotations, "extends=") == klass.name;
}

bool method_needs_user_impl_declaration(
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	if (method.parent != klass.name &&
	    !method_parent_extends_class(classes, klass, method)) {
		return false;
	}
	if (method.is_pure) {
		return false;
	}
	return has_any_annotation(method.annotations, {
	    "local", "localhost", "reply"
	});
}

bool method_needs_kernel_impl_declaration(
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass,
    const MethodInfo &method)
{
	if (method.is_static && method_needs_user_impl_declaration(classes, klass, method)) {
		return has_annotation(klass.annotations, "kernel") && !method.is_pure;
	}
	if (method_needs_user_impl_declaration(classes, klass, method)) {
		return false;
	}
	if (method.parent != klass.name) {
		return true;
	}
	return has_annotation(klass.annotations, "kernel") && !method.is_pure;
}

bool method_overrides_rpc_root(const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	const ClassInfo *root_class = nullptr;
	const MethodInfo *root = find_root_method(classes, klass, method, &root_class);
	return root != &method || root_class != &klass;
}

bool method_needs_interface_declaration(const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	if (!method.is_virtual || !has_annotation(method.annotations, "localonly")) {
		return false;
	}
	const ClassInfo *root_class = nullptr;
	const MethodInfo *root = find_root_method(classes, klass, method, &root_class);
	return root == &method && root_class == &klass;
}

void write_impl_declaration(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method,
    const std::string &suffix = "_Impl")
{
	const ClassInfo *root_class = nullptr;
	find_root_method(classes, klass, method, &root_class);
	std::string args_class = root_class ? root_class->name : klass.name;
	out << "    ";
	if (method.is_static) {
		out << "static ";
	}
	out << method.result_type << "\\\n";
	out << "    " << method.name << suffix << "("
	    << args_class << "_" << method.name << "_Args)";
	if (method.is_const) {
		out << " const";
	}
	out << ";\\\n";
	write_macro_blank(out);
}

void write_methods_macro(std::ostringstream &out, const ClassInfo &klass,
    const std::map<std::string, const ClassInfo *> &classes,
    const std::vector<const MethodInfo *> &methods,
    const std::vector<const MethodInfo *> &rpc_methods)
{
	std::set<std::string> typed_action_targets;
	for (const MethodInfo *method : methods) {
		for (const MethodInfo::ParamInfo &param : method->params) {
			if (!param.type_method_name.empty()) {
				typed_action_targets.insert(param.type_method_name);
			}
		}
	}
	out << "#define " << klass.name << "_Methods \\\n";
	write_macro_blank(out);
	out << "public:\\\n";
	write_macro_blank(out);
	if (!rpc_methods.empty() || has_any_annotation(klass.annotations, {
	    "kernel", "native", "serializable", "concrete"
	})) {
		out << "    virtual kern_return_t\\\n";
		out << "    Dispatch(const IORPC rpc) APPLE_KEXT_OVERRIDE;\\\n";
		write_macro_blank(out);
		out << "    static kern_return_t\\\n";
		out << "    _Dispatch(" << klass.name << " * self, const IORPC rpc);\\\n";
		write_macro_blank(out);
	}
	for (const MethodInfo *method : methods) {
		if (is_rpc_method(classes, klass, *method) &&
		    method_overrides_rpc_root(classes, klass, *method)) {
			continue;
		}
		if (method_has_type_annotation(*method)) {
			out << "    kern_return_t\\\n";
			out << "    CreateAction" << method->name
			    << "(size_t referenceSize, OSAction ** action);\\\n";
		} else {
			bool include_supermethod = is_rpc_method(classes, klass, *method) &&
			    !method->is_static;
			write_wrapped_method_decl(out, classes, *method, method->name, false,
			    method->is_static, include_supermethod, false);
		}
		write_macro_blank(out);
	}
	write_macro_blank(out);
	out << "protected:\\\n";
	out << "    /* _Impl methods */\\\n";
	write_macro_blank(out);
	for (const MethodInfo *method : rpc_methods) {
		if (typed_action_targets.find(method->name) != typed_action_targets.end() &&
		    !has_annotation(method->annotations, "reply")) {
			continue;
		}
		if (!method_needs_user_impl_declaration(classes, klass, *method)) {
			continue;
		}
		write_impl_declaration(out, classes, klass, *method,
		    method->is_static ? "_Call" : "_Impl");
	}
	write_macro_blank(out);
	out << "public:\\\n";
	out << "    /* _Invoke methods */\\\n";
	write_macro_blank(out);
	for (const MethodInfo *method : rpc_methods) {
		if (method_has_type_annotation(*method)) {
			continue;
		}
		if (method_overrides_rpc_root(classes, klass, *method)) {
			continue;
		}
		out << "    typedef " << method->result_type << " (*"
		    << method->name << "_Handler)(";
		if (!method->is_static) {
			out << "OSMetaClassBase * target";
			if (!method->params.empty()) {
				out << ", ";
			}
		}
		out << klass.name << "_" << method->name << "_Args);\\\n";
		if (!method->is_static &&
		    method_has_osaction_target_param(classes, *method)) {
			out << "    static kern_return_t\\\n";
			out << "    " << method->name << "_Invoke(const IORPC rpc,\\\n";
			out << "        OSMetaClassBase * target,\\\n";
			out << "        " << method->name << "_Handler func,\\\n";
			out << "        const OSMetaClass * targetActionClass);\\\n";
			write_macro_blank(out);
		}
		out << "    static kern_return_t\\\n";
		out << "    " << method->name << "_Invoke(const IORPC rpc,\\\n";
		if (!method->is_static) {
			out << "        OSMetaClassBase * target,\\\n";
		}
		out << "        " << method->name << "_Handler func);\\\n";
		write_macro_blank(out);
	}
	out << "\n\n";
}

void write_kernel_methods_macro(std::ostringstream &out, const ClassInfo &klass,
    const std::map<std::string, const ClassInfo *> &classes,
    const std::vector<const MethodInfo *> &rpc_methods)
{
	out << "#define " << klass.name << "_KernelMethods \\\n";
	write_macro_blank(out);
	out << "protected:\\\n";
	out << "    /* _Impl methods */\\\n";
	write_macro_blank(out);
	for (const MethodInfo *method : rpc_methods) {
		if (!method_needs_kernel_impl_declaration(classes, klass, *method)) {
			continue;
		}
		write_impl_declaration(out, classes, klass, *method);
	}
	out << "\n\n";
}

bool write_method_ids(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const std::vector<const MethodInfo *> &methods)
{
	bool wrote = false;
	for (const MethodInfo *method : methods) {
		const ClassInfo *root_class = nullptr;
		const MethodInfo *root = find_root_method(classes, klass, *method, &root_class);
		if (root != method || root_class != &klass) {
			continue;
		}
		out << "#define " << klass.name << "_" << method->name
		    << "_ID            " << hex_u64(method_id_for(klass, *method)) << "\n";
		wrote = true;
	}
	if (wrote) {
		out << "\n";
	}
	return wrote;
}

void write_method_args_macro(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	out << "#define " << klass.name << "_" << method.name << "_Args \\\n";
	bool include_rpc = has_annotation(method.annotations, "invokereply") ||
	    class_or_base_method_has_annotation(classes, klass.base, method.name,
	    "invokereply");
	if (include_rpc) {
		out << "        const IORPC rpc";
		if (!method.params.empty()) {
			out << ", \\";
		}
		out << "\n";
	}
	for (std::size_t i = 0; i < method.params.size(); ++i) {
		out << "        " << declaration_param_type(classes, method.params[i]);
		if (!method.params[i].name.empty()) {
			out << " " << method.params[i].name;
		}
		if (i + 1 != method.params.size()) {
			out << ", \\";
		}
		out << "\n";
	}
	if (!include_rpc && method.params.empty()) {
		out << "\n";
	}
	out << "\n";
}

void write_method_args(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass,
    const std::vector<const MethodInfo *> &methods)
{
	for (const MethodInfo *method : methods) {
		write_method_args_macro(out, classes, klass, *method);
	}
}

void write_method_macro(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const std::string &suffix,
    const std::vector<const MethodInfo *> &methods, bool virtual_override)
{
	out << "#define " << klass.name << "_" << suffix;
	if (methods.empty()) {
		if (virtual_override) {
			out << " \\\n";
			write_macro_blank(out);
			out << "public:\\\n";
			write_macro_blank(out);
			out << "\n";
			return;
		}
		out << "\n\n";
		return;
	}
	out << " \\\n";
	write_macro_blank(out);
	out << "public:\\\n";
	if (virtual_override) {
		write_macro_blank(out);
		for (const MethodInfo *method : methods) {
			write_wrapped_method_decl(out, classes, *method, method->name, true,
			    false, false, true);
			write_macro_blank(out);
		}
	} else {
		for (std::size_t i = 0; i < methods.size(); ++i) {
			write_macro_line(out, "    " + method_declaration(*methods[i], virtual_override),
			    i + 1 != methods.size());
		}
	}
	out << "\n";
}

void write_interface_method(std::ostringstream &out, const MethodInfo &method, bool call_wrapper)
{
	out << "    ";
	if (!call_wrapper) {
		out << "virtual ";
	}
	out << method.result_type << "\n";
	out << "    " << method.name;
	if (call_wrapper) {
		out << "_Call";
	}
	out << "(";
	if (!method.params.empty()) {
		for (std::size_t i = 0; i < method.params.size(); ++i) {
			if (i != 0) {
				out << "        ";
			}
			out << method.params[i].type;
			if (!method.params[i].name.empty()) {
				out << " " << method.params[i].name;
			}
			if (i + 1 != method.params.size()) {
				out << ",";
			}
			if (i + 1 != method.params.size()) {
				out << "\n";
			}
		}
	}
	out << ")";
	if (method.is_const) {
		out << " const";
	}
	if (!method.suffix_attributes.empty()) {
		out << " " << method.suffix_attributes;
	}
	if (!call_wrapper) {
		out << " = 0;\n\n";
		return;
	}
	out << "  { ";
	out << "return " << method.name << "(";
	for (std::size_t i = 0; i < method.params.size(); ++i) {
		if (i != 0) {
			out << ", ";
		}
		out << method.params[i].name;
	}
	out << "); };\\\n\n";
}

void write_interface_methods(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const std::vector<const MethodInfo *> &methods)
{
	std::vector<const MethodInfo *> interface_methods;
	for (const MethodInfo *method : methods) {
		if (method_needs_interface_declaration(classes, klass, *method)) {
			interface_methods.push_back(method);
		}
	}
	for (const MethodInfo *method : interface_methods) {
		write_interface_method(out, *method, false);
	}
	for (const MethodInfo *method : interface_methods) {
		write_interface_method(out, *method, true);
	}
}

void write_generated_class(std::ostringstream &out, const ClassInfo &klass,
    const ParseState &state,
    const std::map<std::string, const ClassInfo *> &classes,
    const std::string &source_name, unsigned source_start)
{
	std::string extends = annotation_value(klass.annotations, "extends=");
	if (!extends.empty()) {
		out << "/* generated extension " << klass.name << " "
		    << source_name << ":" << klass.start_line
		    << "-" << klass.end_line << " */\n\n";
		return;
	}

	std::vector<const MethodInfo *> all_methods;
	std::vector<const MethodInfo *> virtual_methods;
	for (const MethodInfo *method : generated_methods_for_class(state, klass)) {
		if ((method->is_virtual && method_is_localonly(classes, klass, *method)) ||
		    method_needs_interface_declaration(classes, klass, *method)) {
			virtual_methods.push_back(method);
		} else {
			all_methods.push_back(method);
		}
	}

	out << "/* generated class " << klass.name << " "
	    << source_name << ":" << source_start << "-" << (klass.end_line - 1)
	    << " */\n\n";
	std::vector<const MethodInfo *> rpc_methods = rpc_methods_for_class(state, klass);
	bool wrote_method_ids = write_method_ids(out, classes, klass, rpc_methods);
	if (!wrote_method_ids) {
		out << "\n";
	}
	write_method_args(out, classes, klass, rpc_methods);
	write_methods_macro(out, klass, classes, all_methods, rpc_methods);
	write_kernel_methods_macro(out, klass, classes, rpc_methods);
	write_method_macro(out, classes, klass, "VirtualMethods", virtual_methods, true);

	out << "\n#if !KERNEL\n\n"
	    << "extern OSMetaClass          * g" << klass.name << "MetaClass;\n"
	    << "extern const OSClassLoadInformation " << klass.name << "_Class;\n\n"
	    << "class " << klass.name << "MetaClass : public OSMetaClass\n"
	    << "{\n"
	    << "public:\n"
	    << "    virtual kern_return_t\n"
	    << "    New(OSObject * instance) override;\n"
	    << "    virtual kern_return_t\n"
	    << "    Dispatch(const IORPC rpc) override;\n"
	    << "};\n\n"
	    << "#endif /* !KERNEL */\n\n";

	bool kernel_visible = has_annotation(klass.annotations, "native");
	if (!kernel_visible) {
		out << "#if !KERNEL\n\n";
	}
	out << "class  " << klass.name << "Interface : public OSInterface\n"
	    << "{\n"
	    << "public:\n";
	write_interface_methods(out, classes, klass, generated_methods_for_class(state, klass));
	out << "};\n\n";
	out << "struct " << klass.name << "_IVars;\n";
	out << "struct " << klass.name << "_LocalIVars;\n\n";
	out << "class " << klass.name << " : public "
	    << (klass.base.empty() ? "OSObject" : klass.base)
	    << ", public " << klass.name << "Interface\n"
	    << "{\n";
	if (kernel_visible) {
		out << "#if KERNEL\n"
		    << "    OSDeclareDefaultStructorsWithDispatch(" << klass.name << ");\n"
		    << "#endif /* KERNEL */\n\n";
	}
	out << "#if !KERNEL\n"
	    << "    friend class " << klass.name << "MetaClass;\n"
	    << "#endif /* !KERNEL */\n\n";
	if (!kernel_visible) {
		out << "#if !KERNEL\n";
	}
	out << "public:\n"
	    << "#ifdef " << klass.name << "_DECLARE_IVARS\n"
	    << klass.name << "_DECLARE_IVARS\n"
	    << "#else /* " << klass.name << "_DECLARE_IVARS */\n"
	    << "    union\n"
	    << "    {\n"
	    << "        " << klass.name << "_IVars * ivars;\n"
	    << "        " << klass.name << "_LocalIVars * lvars;\n"
	    << "    };\n"
	    << "#endif /* " << klass.name << "_DECLARE_IVARS */\n";
	if (!kernel_visible) {
		out << "#endif /* !KERNEL */\n\n";
	}
	out << "#if !KERNEL\n"
	    << "    static OSMetaClass *\n"
	    << "    sGetMetaClass() { return g" << klass.name << "MetaClass; };\n";
	if (!class_has_metaclass_base(klass) && kernel_visible) {
		out << "    virtual const OSMetaClass *\n"
		    << "    getMetaClass() const APPLE_KEXT_OVERRIDE { return g"
		    << klass.name << "MetaClass; };\n";
	}
	out << "#endif /* KERNEL */\n\n"
	    << "    using super = " << (klass.base.empty() ? "OSObject" : klass.base) << ";\n\n"
	    << "#if !KERNEL\n"
	    << "    " << klass.name << "_Methods\n";
	if (!kernel_visible) {
		out << "    " << klass.name << "_VirtualMethods\n";
	}
	out << "#endif /* !KERNEL */\n\n";
	if (kernel_visible) {
		out << "    " << klass.name << "_VirtualMethods\n";
	}
	out << "};\n";
	if (!kernel_visible) {
		out << "#endif /* !KERNEL */\n\n";
	}
	out << "\n";
}

void write_action_class_for_type_method(std::ostringstream &out,
    const ClassInfo &klass, const MethodInfo &method)
{
	std::string action_class = action_class_name(klass, method);
	const char *availability =
	    "__attribute__((availability(driverkit,introduced=20,message=\"Type-safe OSAction factory methods are available in DriverKit 20 and newer\")))";

	out << "#define " << action_class << "_Methods \\\n";
	write_macro_blank(out);
	out << "public:\\\n";
	write_macro_blank(out);
	out << "    virtual kern_return_t\\\n";
	out << "    Dispatch(const IORPC rpc) APPLE_KEXT_OVERRIDE;\\\n";
	write_macro_blank(out);
	out << "    static kern_return_t\\\n";
	out << "    _Dispatch(" << action_class << " * self, const IORPC rpc);\\\n";
	write_macro_blank(out);
	write_macro_blank(out);
	out << "protected:\\\n";
	out << "    /* _Impl methods */\\\n";
	write_macro_blank(out);
	write_macro_blank(out);
	out << "public:\\\n";
	out << "    /* _Invoke methods */\\\n";
	write_macro_blank(out);
	out << "\n\n";

	out << "#define " << action_class << "_KernelMethods \\\n";
	write_macro_blank(out);
	out << "protected:\\\n";
	out << "    /* _Impl methods */\\\n";
	write_macro_blank(out);
	out << "\n\n";

	out << "#define " << action_class << "_VirtualMethods \\\n";
	write_macro_blank(out);
	out << "public:\\\n";
	write_macro_blank(out);
	out << "\n\n";

	out << "#if !KERNEL\n\n";
	out << "extern OSMetaClass          * g" << action_class << "MetaClass;\n";
	out << "extern const OSClassLoadInformation " << action_class << "_Class;\n\n";
	out << "class " << action_class << "MetaClass : public OSMetaClass\n";
	out << "{\n";
	out << "public:\n";
	out << "    virtual kern_return_t\n";
	out << "    New(OSObject * instance) override;\n";
	out << "    virtual kern_return_t\n";
	out << "    Dispatch(const IORPC rpc) override;\n";
	out << "};\n\n";
	out << "#endif /* !KERNEL */\n\n";

	out << "class  " << availability << " " << action_class
	    << "Interface : public OSInterface\n";
	out << "{\n";
	out << "public:\n";
	out << "};\n\n";
	out << "struct " << action_class << "_IVars;\n";
	out << "struct " << action_class << "_LocalIVars;\n\n";
	out << "class " << availability << " " << action_class
	    << " : public OSAction, public " << action_class << "Interface\n";
	out << "{\n";
	out << "#if KERNEL\n";
	out << "    OSDeclareDefaultStructorsWithDispatch(" << action_class << ");\n";
	out << "#endif /* KERNEL */\n\n";
	out << "#if !KERNEL\n";
	out << "    friend class " << action_class << "MetaClass;\n";
	out << "#endif /* !KERNEL */\n\n";
	out << "public:\n";
	out << "#ifdef " << action_class << "_DECLARE_IVARS\n";
	out << action_class << "_DECLARE_IVARS\n";
	out << "#else /* " << action_class << "_DECLARE_IVARS */\n";
	out << "    union\n";
	out << "    {\n";
	out << "        " << action_class << "_IVars * ivars;\n";
	out << "        " << action_class << "_LocalIVars * lvars;\n";
	out << "    };\n";
	out << "#endif /* " << action_class << "_DECLARE_IVARS */\n";
	out << "#if !KERNEL\n";
	out << "    static OSMetaClass *\n";
	out << "    sGetMetaClass() { return g" << action_class << "MetaClass; };\n";
	out << "    virtual const OSMetaClass *\n";
	out << "    getMetaClass() const APPLE_KEXT_OVERRIDE { return g"
	    << action_class << "MetaClass; };\n";
	out << "#endif /* KERNEL */\n\n";
	out << "    using super = OSAction;\n\n";
	out << "#if !KERNEL\n";
	out << "    " << action_class << "_Methods\n";
	out << "#endif /* !KERNEL */\n\n";
	out << "    " << action_class << "_VirtualMethods\n";
	out << "};\n\n";
}

void write_action_classes_for_type_methods(std::ostringstream &out, const ClassInfo &klass)
{
	for (const MethodInfo &method : klass.methods) {
		if (method_has_type_annotation(method)) {
			write_action_class_for_type_method(out, klass, method);
		}
	}
}

std::vector<std::string> action_class_names_for_type_methods(const ClassInfo &klass)
{
	std::vector<std::string> names;
	for (const MethodInfo &method : klass.methods) {
		if (method_has_type_annotation(method)) {
			names.push_back(action_class_name(klass, method));
		}
	}
	return names;
}

std::vector<const ClassInfo *> generated_main_classes(const ParseState &state)
{
	std::vector<const ClassInfo *> classes;
	for (const ClassInfo &klass : state.classes) {
		if (klass.from_main_file && is_generated_header_class(klass) &&
		    annotation_value(klass.annotations, "extends=").empty()) {
			classes.push_back(&klass);
		}
	}
	return classes;
}

std::set<std::string> typed_action_target_names(const std::vector<const MethodInfo *> &methods)
{
	std::set<std::string> targets;
	for (const MethodInfo *method : methods) {
		for (const MethodInfo::ParamInfo &param : method->params) {
			if (!param.type_method_name.empty()) {
				targets.insert(param.type_method_name);
			}
		}
	}
	return targets;
}

bool method_is_local_dispatch(const MethodInfo &method)
{
	return has_any_annotation(method.annotations, {
	    "local", "localhost", "reply"
	});
}

bool method_is_invoke_reply(const MethodInfo &method)
{
	return has_annotation(method.annotations, "invokereply");
}

bool class_has_metaclass_base(const ClassInfo &klass)
{
	return klass.base == "OSMetaClassBase";
}

bool method_needs_instance_dispatch_case(const ClassInfo &klass,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method, const std::set<std::string> &typed_action_targets)
{
	if (class_has_metaclass_base(klass) || method.is_static ||
	    method_has_type_annotation(method)) {
		return false;
	}
	if (method.parent == klass.name && method.is_pure) {
		return false;
	}
	if (typed_action_targets.find(method.name) != typed_action_targets.end() &&
	    !has_annotation(method.annotations, "reply")) {
		return false;
	}
	return is_rpc_method(classes, klass, method);
}

bool method_needs_metaclass_dispatch_case(const ClassInfo &klass,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method)
{
	return method.is_static && is_rpc_method(classes, klass, method) &&
	    !method_has_type_annotation(method);
}

bool method_needs_impl_wrapper(const ClassInfo &klass, const MethodInfo &method)
{
	if (!method.is_static) {
		return true;
	}
	return method.parent == klass.name;
}

std::string strip_param_type_prefixes(std::string type)
{
	while (type.rfind("const ", 0) == 0) {
		type = type.substr(6);
	}
	while (type.size() >= 6 && type.compare(type.size() - 6, 6, " const") == 0) {
		type.resize(type.size() - 6);
	}
	while (type.size() >= 5 && type.compare(type.size() - 5, 5, "const") == 0) {
		std::size_t before = type.size() - 5;
		if (before == 0 ||
		    std::isalnum(static_cast<unsigned char>(type[before - 1])) ||
		    type[before - 1] == '_') {
			break;
		}
		type.resize(before);
		while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) {
			type.pop_back();
		}
	}
	return type;
}

bool type_has_leading_const(const std::string &type)
{
	std::string normalized = trim_copy(type);
	return normalized.rfind("const ", 0) == 0;
}

std::string strip_top_level_const(std::string type)
{
	type = trim_copy(type);
	while (type.size() >= 6 && type.compare(type.size() - 6, 6, " const") == 0) {
		type.resize(type.size() - 6);
		type = trim_copy(type);
	}
	while (type.size() >= 5 && type.compare(type.size() - 5, 5, "const") == 0) {
		std::size_t before = type.size() - 5;
		if (before == 0 ||
		    std::isalnum(static_cast<unsigned char>(type[before - 1])) ||
		    type[before - 1] == '_') {
			break;
		}
		type.resize(before);
		type = trim_copy(type);
	}
	return type;
}

std::string pointee_type(const std::string &type)
{
	std::string out = strip_param_type_prefixes(type);
	while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
		out.pop_back();
	}
	if (!out.empty() && out.back() == '*') {
		out.pop_back();
	}
	while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
		out.pop_back();
	}
	return out;
}

std::string strip_type_tag_prefix(std::string type)
{
	type = trim_copy(type);
	for (const char *prefix : {"class ", "struct "}) {
		if (type.rfind(prefix, 0) == 0) {
			return type.substr(std::strlen(prefix));
		}
	}
	return type;
}

std::string strip_type_tag_keywords(std::string type)
{
	for (const char *keyword : {"class ", "struct "}) {
		std::size_t pos = 0;
		while ((pos = type.find(keyword, pos)) != std::string::npos) {
			bool at_token_start = pos == 0 ||
			    std::isspace(static_cast<unsigned char>(type[pos - 1])) ||
			    type[pos - 1] == '*';
			if (at_token_start) {
				type.erase(pos, std::strlen(keyword));
			} else {
				pos += std::strlen(keyword);
			}
		}
	}
	return type;
}

std::string object_class_name_for_type(const std::map<std::string, const ClassInfo *> &classes,
    const std::string &type)
{
	if (type_has_leading_const(type)) {
		return "";
	}
	std::string name = strip_type_tag_prefix(pointee_type(strip_top_level_const(type)));
	if (!name.empty() && name.back() == '*') {
		name = strip_type_tag_prefix(pointee_type(name));
	}
	auto it = classes.find(name);
	if (it != classes.end() && !it->second->is_struct) {
		return name;
	}
	return "";
}

std::string object_class_name_for_param(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	if (param.class_facts_resolved) {
		return param.object_class_name;
	}
	std::string name = object_class_name_for_type(classes, param.type);
	if (!name.empty()) {
		return name;
	}
	if (!param.canonical_type.empty() && param.canonical_type != param.type) {
		return object_class_name_for_type(classes, param.canonical_type);
	}
	return "";
}

bool param_is_object_pointer(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	return !object_class_name_for_param(classes, param).empty();
}

bool param_is_serializable_object_pointer(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	if (param.class_facts_resolved) {
		return param.object_class_is_serializable;
	}
	std::string object_class = object_class_name_for_param(classes, param);
	auto it = classes.find(object_class);
	return it != classes.end() &&
	    has_annotation(it->second->annotations, "serializable");
}

bool param_is_object_array(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param);
bool param_is_input_object_array(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param);

std::string declaration_param_type(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	if (!param.array_count.empty() &&
	    param_is_serializable_object_pointer(classes, param)) {
		return "const " + object_class_name_for_param(classes, param) + " *";
	}
	if (param_is_object_array(classes, param)) {
		if (param_is_input_object_array(classes, param)) {
			return object_class_name_for_param(classes, param) + " ** const";
		}
		return object_class_name_for_param(classes, param) + " **";
	}
	return param.type;
}

bool param_is_object_array(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	return !param.array_count.empty() &&
	    param_is_object_pointer(classes, param) &&
	    !param_is_serializable_object_pointer(classes, param);
}

bool param_is_input_object_array(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	return param_is_object_array(classes, param) &&
	    param.referent_is_const;
}

std::string param_port_disposition(const MethodInfo::ParamInfo &param)
{
	return param.port_disposition;
}

bool param_is_port_descriptor(const MethodInfo::ParamInfo &param)
{
	return !param_port_disposition(param).empty();
}

bool param_is_pointer(const MethodInfo::ParamInfo &param)
{
	return param.pointer_depth > 0 || param.is_array_type;
}

bool param_is_inline_string(const MethodInfo::ParamInfo &param)
{
	return param.referent_is_const &&
	    param.referent_is_char &&
	    !param.array_count.empty();
}

bool param_is_unbounded_const_char_pointer(const MethodInfo::ParamInfo &param)
{
	return param.referent_is_const &&
	    param.referent_is_char &&
	    param.array_count.empty();
}

bool param_is_const_value_pointer(const MethodInfo::ParamInfo &param)
{
	return param.referent_is_const &&
	    param.array_count.empty() &&
	    param_is_pointer(param) &&
	    !param_is_unbounded_const_char_pointer(param);
}

bool param_is_output(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	if (param_is_object_pointer(classes, param)) {
		if (!param.array_count.empty() &&
		    param_is_serializable_object_pointer(classes, param)) {
			return false;
		}
		if (param_is_input_object_array(classes, param)) {
			return false;
		}
		if (param_is_object_array(classes, param)) {
			return true;
		}
		return param.pointer_depth >= 2;
	}
	if (param_is_unbounded_const_char_pointer(param) || param.referent_is_const) {
		return false;
	}
	return param_is_pointer(param);
}

std::string inline_string_bound(const MethodInfo::ParamInfo &param)
{
	return param.array_count.empty() ? "256" : param.array_count;
}

bool type_is_struct(const std::map<std::string, const ClassInfo *> &classes,
    const std::string &type)
{
	auto it = classes.find(strip_param_type_prefixes(type));
	return it != classes.end() && it->second->is_struct;
}

bool param_is_const_struct_pointer(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	return param.referent_is_const &&
	    param.array_count.empty() &&
	    param_is_pointer(param) &&
	    (param.class_facts_resolved ? param.pointee_is_struct :
	    type_is_struct(classes,
	        pointee_type(param.canonical_type.empty() ? param.type : param.canonical_type)));
}

bool param_is_output_array(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	return !param.array_count.empty() &&
	    param_is_output(classes, param) &&
	    !param_is_object_array(classes, param);
}

bool param_is_inline_array(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	return !param.array_count.empty() &&
	    !param_is_object_array(classes, param) &&
	    !param_is_serializable_object_pointer(classes, param) &&
	    !param_is_inline_string(param);
}

const MethodInfo::ParamInfo *param_by_name(const MethodInfo &method,
    const std::string &name)
{
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (param.name == name) {
			return &param;
		}
	}
	return nullptr;
}

const MethodInfo::ParamInfo *array_param_for_count(
    const MethodInfo &method,
    const MethodInfo::ParamInfo &param)
{
	if (param.associated_array_param_name.empty()) {
		return nullptr;
	}
	return param_by_name(method, param.associated_array_param_name);
}

const MethodInfo::ParamInfo *output_array_param_for_count(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method,
    const MethodInfo::ParamInfo &param)
{
	const MethodInfo::ParamInfo *array_param = array_param_for_count(method,
	    param);
	if (array_param != nullptr &&
	    param_is_output(classes, *array_param) &&
	    (param_is_output_array(classes, *array_param) ||
	    param_is_object_array(classes, *array_param))) {
		return array_param;
	}
	return nullptr;
}

const MethodInfo::ParamInfo *input_array_param_for_count(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method,
    const MethodInfo::ParamInfo &param)
{
	const MethodInfo::ParamInfo *array_param = array_param_for_count(method,
	    param);
	if (array_param != nullptr &&
	    (param_is_object_array(classes, *array_param) ||
	    param_is_inline_array(classes, *array_param) ||
	    (!array_param->array_count.empty() &&
	    param_is_serializable_object_pointer(classes, *array_param))) &&
	    !param_is_output(classes, *array_param)) {
		return array_param;
	}
	return nullptr;
}

bool param_appears_before(const MethodInfo &method, const std::string &left,
    const std::string &right)
{
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (param.name == left) {
			return true;
		}
		if (param.name == right) {
			return false;
		}
	}
	return false;
}

const MethodInfo::ParamInfo *output_count_param_for_array(
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method,
    const MethodInfo::ParamInfo &array_param)
{
	(void)classes;
	if (array_param.associated_count_param_name.empty()) {
		return nullptr;
	}
	return param_by_name(method, array_param.associated_count_param_name);
}

std::string canonical_pointee_type(const MethodInfo::ParamInfo &param)
{
	if (!param.canonical_type.empty()) {
		return pointee_type(param.canonical_type);
	}
	return pointee_type(param.type);
}

std::string canonical_param_type(const MethodInfo::ParamInfo &param)
{
	if (!param.canonical_type.empty()) {
		return strip_type_tag_keywords(param.canonical_type);
	}
	return param.type;
}

std::string rpc_field_type_for_param(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo::ParamInfo &param)
{
	if (param_is_serializable_object_pointer(classes, param)) {
		if (param_is_output(classes, param)) {
			return object_class_name_for_param(classes, param) + " *";
		}
		if (!param.array_count.empty()) {
			return object_class_name_for_param(classes, param) + " *";
		}
		if (param.referent_is_const) {
			return object_class_name_for_param(classes, param);
		}
		return object_class_name_for_param(classes, param) + " *";
	}
	if (param_is_object_pointer(classes, param)) {
		return "OSObjectRef";
	}
	if (param_is_port_descriptor(param)) {
		return "";
	}
	if (param_is_const_value_pointer(param)) {
		return canonical_pointee_type(param);
	}
	if (param_is_const_struct_pointer(classes, param)) {
		return pointee_type(param.type);
	}
	if (param_is_output(classes, param)) {
		return canonical_pointee_type(param);
	}
	if (param_is_unbounded_const_char_pointer(param)) {
		return "char";
	}
	if (param_is_inline_string(param)) {
		return "const char *";
	}
	return param.type;
}

unsigned rpc_object_ref_count(const std::map<std::string, const ClassInfo *> &classes,
    const std::vector<MethodInfo::ParamInfo> &params, bool output)
{
	unsigned refs = 0;
	for (const MethodInfo::ParamInfo &param : params) {
		if (param_is_output(classes, param) != output) {
			continue;
		}
		if (param_is_object_array(classes, param)) {
			refs += static_cast<unsigned>(std::strtoul(param.array_count.c_str(),
			    nullptr, 10));
		} else if (param_is_object_pointer(classes, param)) {
			++refs;
		}
	}
	return refs;
}

void write_rpc_content_fields(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method, bool output)
{
	for (int pass = 0; pass < 2; ++pass) {
	for (const MethodInfo::ParamInfo &param : method.params) {
		bool output_array_count =
		    output_array_param_for_count(classes, method, param) != nullptr;
		if ((param_is_output(classes, param) != output &&
		    !output_array_count) ||
		    param_is_object_pointer(classes, param) != (pass == 0)) {
			continue;
		}
		if (param_is_port_descriptor(param)) {
			continue;
		}
		if (param_is_object_array(classes, param)) {
			out << "    OSObjectRef __" << param.name << "["
			    << param.array_count << "];\n";
			continue;
		}
		if (output && param_is_output_array(classes, param)) {
			out << "    " << param.type << "  " << param.name << ";\n";
			out << "#if !defined(__LP64__)\n";
			out << "    uint32_t __" << param.name << "Pad;\n";
			out << "#endif /* !defined(__LP64__) */\n";
			out << "    " << pointee_type(param.type) << " __"
			    << param.name << "[" << param.array_count << "];\n";
			continue;
		}
		std::string field_type = rpc_field_type_for_param(classes, param);
		out << "    " << field_type;
		if (param_is_serializable_object_pointer(classes, param)) {
			out << " ";
		} else {
			out << "  ";
		}
		out << param.name << ";\n";
		if (param_is_serializable_object_pointer(classes, param) ||
		    (!output && (param_is_inline_string(param) ||
		    param_is_inline_array(classes, param)))) {
			out << "#if !defined(__LP64__)\n";
			out << "    uint32_t __" << param.name << "Pad;\n";
			out << "#endif /* !defined(__LP64__) */\n";
		}
		if (!output && param_is_inline_string(param)) {
			out << "    char __" << param.name << "["
			    << inline_string_bound(param) << "];\n";
		}
		if (!output && param_is_inline_array(classes, param)) {
			out << "    " << pointee_type(param.type) << " __"
			    << param.name << "[" << param.array_count << "];\n";
		}
	}
	}
}

void write_rpc_descriptors(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const std::vector<MethodInfo::ParamInfo> &params, bool output)
{
	for (const MethodInfo::ParamInfo &param : params) {
		if (param_is_output(classes, param) != output) {
			continue;
		}
		if (param_is_object_array(classes, param)) {
			out << "    mach_msg_port_descriptor_t " << param.name
			    << "__descriptor[" << param.array_count << "];\n";
		} else if (param_is_serializable_object_pointer(classes, param)) {
			out << "    mach_msg_ool_descriptor_t  " << param.name
			    << "__descriptor;\n";
		} else if (param_is_object_pointer(classes, param) ||
		    param_is_port_descriptor(param)) {
			out << "    mach_msg_port_descriptor_t " << param.name
			    << "__descriptor;\n";
		}
	}
}

void write_impl_rpc_types_for_method(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	std::string prefix = klass.name + "_" + method.name;
	unsigned msg_refs = 1 + rpc_object_ref_count(classes, method.params, false);
	unsigned rpl_refs = rpc_object_ref_count(classes, method.params, true);

	out << "struct " << prefix << "_Msg_Content\n";
	out << "{\n";
	out << "    IORPCMessage __hdr;\n";
	out << "    OSObjectRef  __object;\n";
	write_rpc_content_fields(out, classes, method, false);
	out << "};\n";
	out << "#pragma pack(4)\n";
	out << "struct " << prefix << "_Msg\n";
	out << "{\n";
	out << "    IORPCMessageMach           mach;\n";
	out << "    mach_msg_port_descriptor_t __object__descriptor;\n";
	write_rpc_descriptors(out, classes, method.params, false);
	out << "};\n";
	out << "struct " << prefix << "_Msg_With_Content\n";
	out << "{\n";
	out << "    IORPCMessageMach           mach;\n";
	out << "    mach_msg_port_descriptor_t __object__descriptor;\n";
	write_rpc_descriptors(out, classes, method.params, false);
	out << "    " << prefix << "_Msg_Content content;\n";
	out << "};\n";
	out << "#pragma pack()\n";
	out << "#define " << prefix << "_Msg_ObjRefs (" << msg_refs << ")\n\n";

	out << "struct " << prefix << "_Rpl_Content\n";
	out << "{\n";
	out << "    IORPCMessage __hdr;\n";
	write_rpc_content_fields(out, classes, method, true);
	if (method_is_invoke_reply(method)) {
		out << "    uint64_t __replyBuffer[8];\n";
	}
	out << "};\n";
	out << "#pragma pack(4)\n";
	out << "struct " << prefix << "_Rpl\n";
	out << "{\n";
	out << "    IORPCMessageMach           mach;\n";
	write_rpc_descriptors(out, classes, method.params, true);
	out << "};\n";
	out << "struct " << prefix << "_Rpl_With_Content\n";
	out << "{\n";
	out << "    IORPCMessageMach           mach;\n";
	write_rpc_descriptors(out, classes, method.params, true);
	out << "    " << prefix << "_Rpl_Content content;\n";
	out << "};\n";
	out << "#pragma pack()\n";
	out << "#define " << prefix << "_Rpl_ObjRefs (" << rpl_refs << ")\n\n\n";

	out << "typedef union\n";
	out << "{\n";
	out << "    const IORPC rpc;\n";
	out << "    struct\n";
	out << "    {\n";
	out << "#ifdef KERNEL\n";
	out << "        const struct " << prefix << "_Msg * message;\n";
	out << "#else /* KERNEL */\n";
	out << "        const struct " << prefix << "_Msg_With_Content * message;\n";
	out << "#endif /* KERNEL */\n";
	out << "        struct " << prefix << "_Rpl_With_Content       * reply;\n";
	out << "        uint32_t sendSize;\n";
	out << "        uint32_t replySize;\n";
	out << "#ifdef KERNEL\n";
	out << "        const struct " << prefix << "_Msg_Content * kernelContent;\n";
	out << "#endif /* KERNEL */\n";
	out << "    };\n";
	out << "} " << prefix << "_Invocation;\n";
}

void write_impl_rpc_types(std::ostringstream &out, const ParseState &state)
{
	std::map<std::string, const ClassInfo *> classes = class_map(state);
	for (const ClassInfo *klass : generated_main_classes(state)) {
		for (const MethodInfo *method : rpc_methods_for_class(state, *klass)) {
			if (method_has_type_annotation(*method) ||
			    method_overrides_rpc_root(classes, *klass, *method)) {
				continue;
			}
			write_impl_rpc_types_for_method(out, classes, *klass, *method);
		}
	}
}

void append_unique(std::vector<std::string> &values, const std::string &value)
{
	if (value.empty()) {
		return;
	}
	if (std::find(values.begin(), values.end(), value) == values.end()) {
		values.push_back(value);
	}
}

std::vector<std::string> impl_metaclass_externs(const ParseState &state,
    const ClassInfo &klass)
{
	std::vector<std::string> names;
	(void)klass;
	for (const ClassRecord &record : class_records(state)) {
		if (record.name.rfind("OSMetaClass", 0) == 0 ||
		    record.has_iig_impl_metadata) {
			continue;
		}
		append_unique(names, record.name);
	}
	return names;
}

void write_impl_metaclass_declaration(std::ostringstream &out,
    const ParseState &state, const ClassInfo &klass)
{
	out << "#if !KERNEL\n";
	for (const std::string &name : impl_metaclass_externs(state, klass)) {
		out << "extern OSMetaClass * g" << name << "MetaClass;\n";
	}
	out << "#endif /* !KERNEL */\n\n";
}

std::vector<ClassMethodOption> class_method_options(const ParseState &state,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, bool metaclass)
{
	std::vector<ClassMethodOption> options;
	for (const MethodInfo *method : generated_methods_for_class(state, klass)) {
		std::string queue_name =
		    annotation_value(method->annotations, "queuename=");
		if (queue_name.empty() || method->is_static != metaclass ||
		    !is_rpc_method(classes, klass, *method)) {
			continue;
		}
		ClassMethodOption option;
		option.method = method;
		option.queue_name = queue_name;
		options.push_back(option);
	}
	std::sort(options.begin(), options.end(),
	    [&](const ClassMethodOption &left, const ClassMethodOption &right) {
		    const ClassInfo *left_root_class = nullptr;
		    const ClassInfo *right_root_class = nullptr;
		    const MethodInfo *left_root =
		        find_root_method(classes, klass, *left.method, &left_root_class);
		    const MethodInfo *right_root =
		        find_root_method(classes, klass, *right.method, &right_root_class);
		    const ClassInfo &left_class =
		        left_root_class != nullptr ? *left_root_class : klass;
		    const ClassInfo &right_class =
		        right_root_class != nullptr ? *right_root_class : klass;
		    return method_id_for(left_class, *left_root) <
		        method_id_for(right_class, *right_root);
	    });
	return options;
}

std::vector<std::string> class_queue_names(const ParseState &state,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass)
{
	std::vector<std::string> names;
	for (const MethodInfo *method : generated_methods_for_class(state, klass)) {
		std::string queue_name =
		    annotation_value(method->annotations, "queuename=");
		if (!queue_name.empty() && is_rpc_method(classes, klass, *method)) {
			append_unique(names, queue_name);
		}
	}
	return names;
}

void write_c_string_table_define(std::ostringstream &out, const std::string &name,
    const std::vector<std::string> &values)
{
	out << "#define " << name << "  \"\"";
	for (const std::string &value : values) {
		out << " \\\n";
		out << "    \"\\" << std::oct << std::setfill('0') << std::setw(3)
		    << value.size() << std::dec << std::setfill(' ') << value << "\"";
	}
	out << "\n\n";
}

void write_impl_availability(std::ostringstream &out,
    const std::string &availability)
{
	if (!availability.empty()) {
		out << " " << availability << "\n";
	}
}

std::vector<ImplMethodOption> impl_method_option_entries(
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass,
    const std::vector<ClassMethodOption> &options)
{
	std::vector<ImplMethodOption> entries;
	for (const ClassMethodOption &option : options) {
		const ClassInfo *root_class = nullptr;
		const MethodInfo *root = find_root_method(classes, klass,
		    *option.method, &root_class);
		ImplMethodOption entry;
		entry.method_id = (root_class != nullptr ? root_class->name : klass.name) +
		    "_" + root->name + "_ID";
		entry.method_name = option.method->name;
		entry.queue_name = option.queue_name;
		entries.push_back(entry);
	}
	return entries;
}

void write_impl_method_option_entries(std::ostringstream &out,
    const std::vector<std::string> &queue_names,
    const std::vector<ImplMethodOption> &options)
{
	for (const ImplMethodOption &option : options) {
		out << "        " << option.method_id << ",\n";
	}
	for (const ImplMethodOption &option : options) {
		unsigned queue_index = 0;
		for (std::size_t i = 0; i < queue_names.size(); ++i) {
			if (queue_names[i] == option.queue_name) {
				queue_index = static_cast<unsigned>(i);
				break;
			}
		}
		out << "        0x" << std::hex << std::setfill('0') << std::setw(16)
		    << queue_index << std::dec << std::setfill(' ') << ",\n";
	}
}

void write_impl_class_description_records(std::ostringstream &out,
    const ImplClassDescription &desc)
{
	std::vector<std::string> method_names;
	for (const ImplMethodOption &option : desc.method_options) {
		method_names.push_back(option.method_name);
	}
	std::vector<std::string> meta_method_names;
	for (const ImplMethodOption &option : desc.meta_method_options) {
		meta_method_names.push_back(option.method_name);
	}

	write_c_string_table_define(out, desc.name + "_QueueNames", desc.queue_names);
	write_c_string_table_define(out, desc.name + "_MethodNames", method_names);
	write_c_string_table_define(out, desc.name + "MetaClass_MethodNames",
	    meta_method_names);
	out << "struct OSClassDescription_" << desc.name << "_t\n";
	out << "{\n";
	out << "    OSClassDescription base;\n";
	out << "    uint64_t           methodOptions[2 * " << desc.method_options.size() << "];\n";
	out << "    uint64_t           metaMethodOptions[2 * "
	    << desc.meta_method_options.size() << "];\n";
	out << "    char               queueNames[sizeof(" << desc.name << "_QueueNames)];\n";
	out << "    char               methodNames[sizeof(" << desc.name << "_MethodNames)];\n";
	out << "    char               metaMethodNames[sizeof(" << desc.name << "MetaClass_MethodNames)];\n";
	out << "};\n\n";
	write_impl_availability(out, desc.availability);
	out << "const struct OSClassDescription_" << desc.name << "_t\n";
	out << "OSClassDescription_" << desc.name << " =\n";
	out << "{\n";
	out << "    .base =\n";
	out << "    {\n";
	out << "        .descriptionSize         = sizeof(OSClassDescription_" << desc.name << "_t),\n";
	out << "        .name                    = \"" << desc.name << "\",\n";
	out << "        .superName               = \"" << desc.super_name << "\",\n";
	out << "        .methodOptionsSize       = 2 * sizeof(uint64_t) * "
	    << desc.method_options.size() << ",\n";
	out << "        .methodOptionsOffset     = __builtin_offsetof(struct OSClassDescription_"
	    << desc.name << "_t, methodOptions),\n";
	out << "        .metaMethodOptionsSize   = 2 * sizeof(uint64_t) * "
	    << desc.meta_method_options.size() << ",\n";
	out << "        .metaMethodOptionsOffset = __builtin_offsetof(struct OSClassDescription_"
	    << desc.name << "_t, metaMethodOptions),\n";
	out << "        .queueNamesSize       = sizeof(" << desc.name << "_QueueNames),\n";
	out << "        .queueNamesOffset     = __builtin_offsetof(struct OSClassDescription_"
	    << desc.name << "_t, queueNames),\n";
	out << "        .methodNamesSize         = sizeof(" << desc.name << "_MethodNames),\n";
	out << "        .methodNamesOffset       = __builtin_offsetof(struct OSClassDescription_"
	    << desc.name << "_t, methodNames),\n";
	out << "        .metaMethodNamesSize     = sizeof(" << desc.name << "MetaClass_MethodNames),\n";
	out << "        .metaMethodNamesOffset   = __builtin_offsetof(struct OSClassDescription_"
	    << desc.name << "_t, metaMethodNames),\n";
	out << "        .flags                   = " << (desc.can_remote ? "1" : "0")
	    << "*kOSClassCanRemote,\n";
	out << "        .resv1                   = {0},\n";
	out << "    },\n";
	out << "    .methodOptions =\n";
	out << "    {\n";
	write_impl_method_option_entries(out, desc.queue_names, desc.method_options);
	out << "    },\n";
	out << "    .metaMethodOptions =\n";
	out << "    {\n";
	write_impl_method_option_entries(out, desc.queue_names,
	    desc.meta_method_options);
	out << "    },\n";
	out << "    .queueNames      = " << desc.name << "_QueueNames,\n";
	out << "    .methodNames     = " << desc.name << "_MethodNames,\n";
	out << "    .metaMethodNames = " << desc.name << "MetaClass_MethodNames,\n";
	out << "};\n\n";
	write_impl_availability(out, desc.availability);
	out << "OSMetaClass * g" << desc.name << "MetaClass;\n\n";
	write_impl_availability(out, desc.availability);
	out << "static kern_return_t\n";
	out << desc.name << "_New(OSMetaClass * instance);\n\n";
	write_impl_availability(out, desc.availability);
	out << "const OSClassLoadInformation\n";
	out << desc.name << "_Class = \n";
	out << "{\n";
	out << "    .description       = &OSClassDescription_" << desc.name << ".base,\n";
	out << "    .metaPointer       = &g" << desc.name << "MetaClass,\n";
	out << "    .version           = 1,\n";
	out << "    .instanceSize      = sizeof(" << desc.name << "),\n\n";
	out << "    .resv2             = {0},\n\n";
	out << "    .New               = &" << desc.name << "_New,\n";
	out << "    .resv3             = {0},\n\n";
	out << "};\n\n";
	write_impl_availability(out, desc.availability);
	out << "extern const void * const\n";
	out << "g" << desc.name << "_Declaration;\n";
	write_impl_availability(out, desc.availability);
	out << "const void * const\n";
	out << "g" << desc.name << "_Declaration\n";
	write_impl_availability(out, desc.availability);
	out << "__attribute__((used,visibility(\"hidden\"),section(\"__DATA_CONST,__osclassinfo,regular,no_dead_strip\"),no_sanitize(\"address\")))\n";
	out << "    = &" << desc.name << "_Class;\n\n";
	write_impl_availability(out, desc.availability);
	out << "static kern_return_t\n";
	out << desc.name << "_New(OSMetaClass * instance)\n";
	out << "{\n";
	out << "    if (!new(instance) " << desc.name << "MetaClass) return (kIOReturnNoMemory);\n";
	out << "    return (kIOReturnSuccess);\n";
	out << "}\n\n";
	write_impl_availability(out, desc.availability);
	out << "kern_return_t\n";
	out << desc.name << "MetaClass::New(OSObject * instance)\n";
	out << "{\n";
	out << "    if (!new(instance) " << desc.name << ") return (kIOReturnNoMemory);\n";
	out << "    return (kIOReturnSuccess);\n";
	out << "}\n\n";
}

void write_impl_class_description(std::ostringstream &out, const ParseState &state,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass)
{
	std::vector<ClassMethodOption> method_options =
	    class_method_options(state, classes, klass, false);
	std::vector<ClassMethodOption> meta_method_options =
	    class_method_options(state, classes, klass, true);
	if (has_annotation(klass.annotations, "native")) {
		out << "#if KERNEL\n";
		out << "OSDefineMetaClassAndStructors(" << klass.name << ", "
		    << (klass.base.empty() ? "OSObject" : klass.base) << ");\n";
		out << "#endif /* KERNEL */\n\n";
	}

	out << "#if !KERNEL\n\n";
	ImplClassDescription desc;
	desc.name = klass.name;
	desc.super_name = klass.base.empty() ? "OSObject" : klass.base;
	desc.can_remote = has_annotation(klass.annotations, "kernel");
	desc.queue_names = class_queue_names(state, classes, klass);
	desc.method_options = impl_method_option_entries(classes, klass,
	    method_options);
	desc.meta_method_options = impl_method_option_entries(classes, klass,
	    meta_method_options);
	write_impl_class_description_records(out, desc);
	out << "#endif /* !KERNEL */\n\n";
}

void write_dispatch_case(std::ostringstream &out, const ClassInfo &klass,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method, bool metaclass_case)
{
	const ClassInfo *root_class = nullptr;
	find_root_method(classes, klass, method, &root_class);
	std::string dispatch_class = root_class ? root_class->name : klass.name;
	bool guarded = metaclass_case || !method_is_local_dispatch(method);
	if (guarded) {
		out << "#if KERNEL\n";
	}
	out << "        case " << dispatch_class << "_" << method.name << "_ID:\n";
	if (metaclass_case) {
		out << "            ret = " << dispatch_class << "::" << method.name
		    << "_Invoke(rpc, &" << klass.name << "::" << method.name << "_Impl);\n";
	} else {
		out << "        {\n";
		out << "            ret = " << dispatch_class << "::" << method.name
		    << "_Invoke(rpc, self, SimpleMemberFunctionCast(" << dispatch_class
		    << "::" << method.name << "_Handler, *self, &" << klass.name
		    << "::" << method.name << "_Impl));\n";
		out << "            break;\n";
		out << "        }\n";
	}
	if (metaclass_case) {
		out << "            break;\n";
	}
	if (guarded) {
		out << "#endif /* !KERNEL */\n";
	}
}

const MethodInfo *find_method_by_name(const std::vector<const MethodInfo *> &methods,
    const std::string &name)
{
	for (const MethodInfo *method : methods) {
		if (method->name == name) {
			return method;
		}
	}
	return nullptr;
}

void write_typed_action_dispatch_case(std::ostringstream &out,
    const ClassInfo &klass, const MethodInfo &method,
    const MethodInfo &target_method)
{
	out << "#if KERNEL\n";
	out << "        case " << klass.name << "_" << method.name << "_ID:\n";
	out << "        {\n";
	out << "            ret = " << klass.name << "::" << target_method.name
	    << "_Invoke(rpc, self, SimpleMemberFunctionCast(" << klass.name
	    << "::" << target_method.name << "_Handler, *self, &" << klass.name
	    << "::" << method.name << "_Impl), OSTypeID("
	    << action_class_name(klass, method) << "));\n";
	out << "            break;\n";
	out << "        }\n";
	out << "#endif /* !KERNEL */\n";
}

void write_impl_dispatch(std::ostringstream &out, const ClassInfo &klass,
    const ParseState &state,
    const std::map<std::string, const ClassInfo *> &classes)
{
	std::vector<const MethodInfo *> methods = generated_methods_for_class(state, klass);
	std::vector<const MethodInfo *> rpc_methods = rpc_methods_for_class(state, klass);
	std::set<std::string> typed_targets = typed_action_target_names(methods);

	out << "#ifdef KERNEL\n";
	out << "#define MESSAGE_CONTENT(__field) (messageContent->__field)\n";
	out << "#else /* KERNEL */\n";
	out << "#define MESSAGE_CONTENT(__field) (message->content.__field)\n";
	out << "#endif /* KERNEL */\n\n";
	out << "kern_return_t\n";
	out << klass.name << "::Dispatch(const IORPC rpc)\n";
	out << "{\n";
	out << "    return _Dispatch(this, rpc);\n";
	out << "}\n\n";
	out << "kern_return_t\n";
	out << klass.name << "::_Dispatch(" << klass.name << " * self, const IORPC rpc)\n";
	out << "{\n";
	out << "    kern_return_t ret = kIOReturnUnsupported;\n";
	out << "#ifdef KERNEL\n";
	out << "    IORPCMessage * msg = rpc.kernelContent;\n";
	out << "#else /* KERNEL */\n";
	out << "    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);\n";
	out << "#endif /* KERNEL */\n\n";
	out << "    switch (msg->msgid)\n";
	out << "    {\n";
	for (const MethodInfo *method : rpc_methods) {
		if (method_needs_instance_dispatch_case(klass, classes, *method,
		    typed_targets)) {
			write_dispatch_case(out, klass, classes, *method, false);
		} else if (method_has_type_annotation(*method) && !method->is_static) {
			const MethodInfo *target_method = find_method_by_name(rpc_methods,
			    method->type_method_name);
			if (target_method != nullptr) {
				write_typed_action_dispatch_case(out, klass, *method,
				    *target_method);
			}
		}
	}
	out << "\n";
	out << "        default:\n";
	if (class_has_metaclass_base(klass)) {
		out << "            ret = self->OSMetaClassBase::Dispatch(rpc);\n";
	} else {
		out << "            ret = " << (klass.base.empty() ? "OSObject" : klass.base)
		    << "::_Dispatch(self, rpc);\n";
	}
	out << "            break;\n";
	out << "    }\n\n";
	out << "    return (ret);\n";
	out << "}\n\n";

	out << "#if KERNEL\n";
	out << "kern_return_t\n";
	out << klass.name << "::MetaClass::Dispatch(const IORPC rpc)\n";
	out << "{\n";
	out << "#else /* KERNEL */\n";
	out << "kern_return_t\n";
	out << klass.name << "MetaClass::Dispatch(const IORPC rpc)\n";
	out << "{\n";
	out << "#endif /* !KERNEL */\n\n";
	out << "    kern_return_t ret = kIOReturnUnsupported;\n";
	out << "#ifdef KERNEL\n";
	out << "    IORPCMessage * msg = rpc.kernelContent;\n";
	out << "#else /* KERNEL */\n";
	out << "    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);\n";
	out << "#endif /* KERNEL */\n\n";
	out << "    switch (msg->msgid)\n";
	out << "    {\n";
	for (const MethodInfo *method : rpc_methods) {
		if (method_needs_metaclass_dispatch_case(klass, classes, *method)) {
			write_dispatch_case(out, klass, classes, *method, true);
		}
	}
	out << "\n";
	out << "        default:\n";
	out << "            ret = OSMetaClassBase::Dispatch(rpc);\n";
	out << "            break;\n";
	out << "    }\n\n";
	out << "    return (ret);\n";
	out << "}\n\n";
}

std::string impl_wrapper_result_type(const MethodInfo &method)
{
	if (has_annotation(method.annotations, "reply")) {
		return "kern_return_t";
	}
	return method.result_type;
}

std::string impl_wrapper_name(const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	if (method.is_static && method_needs_user_impl_declaration(classes, klass, method)) {
		return method.name + "_Call";
	}
	return method.name;
}

void write_impl_parameter_list(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method, bool include_supermethod, bool include_rpc)
{
	bool wrote = false;
	if (include_rpc) {
		out << "        IORPC rpc";
		wrote = true;
	}
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (wrote) {
			out << ",\n";
		}
		out << "        " << declaration_param_type(classes, param);
		if (!param.name.empty()) {
			out << " " << param.name;
		}
		wrote = true;
	}
	if (include_supermethod) {
		if (wrote) {
			out << ",\n";
		}
		out << "        OSDispatchMethod supermethod";
	}
}

void write_impl_wrapper_signature(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	bool include_supermethod = !method.is_static;
	bool include_rpc = has_annotation(method.annotations, "reply");
	out << impl_wrapper_result_type(method) << "\n";
	if (method.params.empty() && include_supermethod && !include_rpc) {
		out << klass.name << "::" << impl_wrapper_name(classes, klass, method)
		    << "(        OSDispatchMethod supermethod)";
		if (method.is_const) {
			out << " const";
		}
		out << "\n";
		return;
	}
	out << klass.name << "::" << impl_wrapper_name(classes, klass, method) << "(\n";
	write_impl_parameter_list(out, classes, method, include_supermethod, include_rpc);
	out << ")";
	if (method.is_const) {
		out << " const";
	}
	out << "\n";
}

std::string rpc_prefix_for_method(const ClassInfo &klass, const MethodInfo &method)
{
	return klass.name + "_" + method.name;
}

unsigned rpc_message_descriptor_count(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method)
{
	unsigned descriptors = 1 + rpc_object_ref_count(classes, method.params, false);
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (!param_is_output(classes, param) && param_is_port_descriptor(param)) {
			++descriptors;
		}
	}
	return descriptors;
}

unsigned rpc_reply_descriptor_count(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method)
{
	return rpc_object_ref_count(classes, method.params, true);
}

bool method_is_oneway(const MethodInfo &method)
{
	return method.result_type == "void" || has_annotation(method.annotations, "reply");
}

bool method_has_simple_reply(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method)
{
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (!param_is_output(classes, param) &&
		    param_is_serializable_object_pointer(classes, param)) {
			return false;
		}
	}
	return rpc_reply_descriptor_count(classes, method) == 0;
}

void write_impl_wrapper_input_assignment(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method,
    const std::string &prefix, const MethodInfo::ParamInfo &param)
{
	if (output_array_param_for_count(classes, method, param) != nullptr) {
		if (!param_is_output(classes, param)) {
			out << "    msg->content." << param.name << " = "
			    << param.name << ";\n\n";
		}
		return;
	}
	if (param_is_output_array(classes, param)) {
		const MethodInfo::ParamInfo *count_param =
		    output_count_param_for_array(classes, method, param);
		if (count_param != nullptr) {
			out << "    if (*" << count_param->name
			    << " > (sizeof(rpl->content.__" << param.name
			    << ") / sizeof(rpl->content.__" << param.name
			    << "[0]))) return kIOReturnOverrun;\n";
			out << "    msg->content." << count_param->name << " = *"
			    << count_param->name << ";\n\n";
		}
		return;
	}
	if (param_is_output(classes, param) &&
	    param_is_object_array(classes, param)) {
		const MethodInfo::ParamInfo *count_param =
		    output_count_param_for_array(classes, method, param);
		if (count_param != nullptr) {
			out << "    if (*" << count_param->name
			    << " > (sizeof(rpl->content.__" << param.name
			    << ") / sizeof(rpl->content.__" << param.name
			    << "[0]))) return kIOReturnOverrun;\n";
			out << "    msg->content." << count_param->name << " = *"
			    << count_param->name << ";\n\n";
		}
		return;
	}
	if (param_is_output(classes, param)) {
		return;
	}
	if (param_is_object_array(classes, param)) {
		out << "    for (unsigned int idx = 0; idx < " << param.array_count
		    << "; idx++) msg->" << param.name
		    << "__descriptor[idx].type = MACH_MSG_PORT_DESCRIPTOR;\n";
		out << "    if (" << param.name << "Count > (sizeof(msg->content.__"
		    << param.name << ") / sizeof(msg->content.__" << param.name
		    << "[0]))) return kIOReturnOverrun;\n";
		out << "    bcopy(" << param.name << ", &msg->content.__"
		    << param.name << "[0], " << param.name
		    << "Count * sizeof(msg->content.__" << param.name << "[0]));\n\n";
		return;
	}
	if (param_is_serializable_object_pointer(classes, param)) {
		out << "    msg->" << param.name << "__descriptor.type = MACH_MSG_OOL_DESCRIPTOR;\n";
		out << "    msg->" << param.name << "__descriptor.copy = MACH_MSG_VIRTUAL_COPY;\n";
		out << "    msg->" << param.name
		    << "__descriptor.address = (void *) __builtin_offsetof("
		    << prefix << "_Msg_Content, " << param.name << ");\n";
		out << "    msg->content." << param.name << " = " << param.name << ";\n\n";
		if (!param.array_count.empty()) {
			out << "    if (" << param.name << "Count > (sizeof(msg->content.__"
			    << param.name << ") / sizeof(msg->content.__" << param.name
			    << "[0]))) return kIOReturnOverrun;\n";
			out << "    bcopy(" << param.name << ", &msg->content.__"
			    << param.name << "[0], " << param.name
			    << "Count * sizeof(msg->content.__" << param.name << "[0]));\n\n";
		}
		return;
	}
	if (param_is_object_pointer(classes, param)) {
		out << "    msg->" << param.name << "__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;\n";
		out << "    msg->content." << param.name << " = (OSObjectRef) " << param.name << ";\n\n";
		return;
	}
	if (param_is_port_descriptor(param)) {
		out << "    msg->" << param.name << "__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;\n";
		out << "    msg->" << param.name << "__descriptor.disposition = "
		    << param_port_disposition(param) << ";\n";
		out << "    msg->" << param.name << "__descriptor.name = " << param.name << ";\n";
		return;
	}
	if (param_is_inline_string(param)) {
		out << "    msg->content." << param.name << " = NULL;\n\n";
		out << "    strlcpy(&msg->content.__" << param.name << "[0], " << param.name
		    << ", sizeof(msg->content.__" << param.name << "));\n\n";
		return;
	}
	if (param_is_unbounded_const_char_pointer(param)) {
		out << "    msg->content." << param.name << " = *"
		    << param.name << ";\n\n";
		return;
	}
	if (param_is_const_value_pointer(param)) {
		out << "    msg->content." << param.name << " = *"
		    << param.name << ";\n\n";
		return;
	}
	if (param_is_inline_array(classes, param)) {
		out << "    msg->content." << param.name << " = NULL;\n\n";
		out << "    if (" << param.name << "Count > (sizeof(msg->content.__"
		    << param.name << ") / sizeof(msg->content.__" << param.name
		    << "[0]))) ";
		if (method.result_type == "void") {
			out << "return";
		} else {
			out << "return kIOReturnOverrun";
		}
		out << ";\n";
		out << "    bcopy(" << param.name << ", &msg->content.__"
		    << param.name << "[0], " << param.name
		    << "Count * sizeof(msg->content.__" << param.name << "[0]));\n\n";
		return;
	}
	if (param_is_const_struct_pointer(classes, param)) {
		out << "    msg->content." << param.name << " = *"
		    << param.name << ";\n\n";
		return;
	}
	out << "    msg->content." << param.name << " = " << param.name << ";\n\n";
}

void write_impl_wrapper_output_copy(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method,
    const MethodInfo::ParamInfo &param)
{
	if (output_array_param_for_count(classes, method, param) != nullptr) {
		const MethodInfo::ParamInfo *array_param =
		    output_array_param_for_count(classes, method, param);
		if (!param_appears_before(method, array_param->name, param.name)) {
			out << "        if (rpl->content." << param.name << " < *"
			    << param.name << ") *" << param.name << " = rpl->content."
			    << param.name << ";\n";
		}
		return;
	}
	if (param_is_output_array(classes, param)) {
		const MethodInfo::ParamInfo *count_param =
		    output_count_param_for_array(classes, method, param);
		if (count_param != nullptr &&
		    param_appears_before(method, param.name, count_param->name)) {
			out << "        if (rpl->content." << count_param->name << " < *"
			    << count_param->name << ") *" << count_param->name
			    << " = rpl->content." << count_param->name << ";\n";
		}
		out << "        bcopy(&rpl->content.__" << param.name
		    << "[0], " << param.name << ", *"
		    << param.name << "Count * sizeof(rpl->content.__"
		    << param.name << "[0]));\n";
		return;
	}
	if (!param_is_output(classes, param)) {
		return;
	}
	if (param_is_object_array(classes, param)) {
		const MethodInfo::ParamInfo *count_param =
		    output_count_param_for_array(classes, method, param);
		if (count_param != nullptr &&
		    param_appears_before(method, param.name, count_param->name)) {
			out << "        if (rpl->content." << count_param->name << " < *"
			    << count_param->name << ") *" << count_param->name
			    << " = rpl->content." << count_param->name << ";\n";
		}
		if (count_param != nullptr) {
			out << "        for (unsigned int idx = 0; idx < *"
			    << count_param->name << "; idx++)\n";
			out << "        {\n";
			out << "           " << param.name << "[idx] = OSDynamicCast("
			    << object_class_name_for_param(classes, param)
			    << ", (OSObject *) rpl->content.__" << param.name
			    << "[idx]);\n";
			out << "        }\n";
		}
		return;
	}
	if (param_is_object_pointer(classes, param)) {
		std::string object_class = object_class_name_for_param(classes, param);
		out << "        *" << param.name << " = OSDynamicCast(" << object_class
		    << ", (OSObject *) rpl->content." << param.name << ");\n";
		out << "        if (rpl->content." << param.name << " && !*" << param.name
		    << ") ret = kIOReturnBadArgument;\n";
		return;
	}
	out << "        if (" << param.name << ") *" << param.name
	    << " = rpl->content." << param.name << ";\n";
}

void write_impl_wrapper_body(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method)
{
	std::string prefix = rpc_prefix_for_method(klass, method);
	bool oneway = method_is_oneway(method);
	bool include_reply = !oneway;
	const MethodInfo::ParamInfo *target_param = method_target_param(method);
	out << "{\n";
	out << "    kern_return_t ret;\n";
	if (has_annotation(method.annotations, "reply")) {
		out << "    struct " << prefix << "_Msg_With_Content * msg = (typeof(msg)) rpc.reply;\n";
		out << "#ifdef KERNEL\n";
		out << "    rpc.kernelContent = &msg->content.__hdr;\n";
		out << "#endif /* KERNEL */\n\n";
	} else {
		out << "    union\n";
		out << "    {\n";
		out << "        " << prefix << "_Msg_With_Content msg;\n";
		if (include_reply) {
			out << "        struct\n";
			out << "        {\n";
			out << "            " << prefix << "_Rpl_With_Content rpl;\n";
			out << "            mach_msg_max_trailer_t trailer;\n";
			out << "        } rpl;\n";
		}
		out << "    } buf;\n";
		out << "    struct " << prefix << "_Msg_With_Content * msg = &buf.msg;\n";
		if (include_reply) {
			out << "    struct " << prefix << "_Rpl_With_Content * rpl = &buf.rpl.rpl;\n";
		}
		out << "\n";
	}
	out << "    memset(msg, 0, sizeof(*msg));\n";
	out << "    msg->mach.msgh.msgh_id   = kIORPCVersion190615;\n";
	out << "    msg->mach.msgh.msgh_size = sizeof(*msg);\n";
	out << "    msg->content.__hdr.flags = " << (oneway ? 1 : 0) << "*kIORPCMessageOneway\n";
	out << "                             | " << (method_has_simple_reply(classes, method) ? 1 : 0)
	    << "*kIORPCMessageSimpleReply\n";
	out << "                             | " << (has_annotation(method.annotations, "localhost") ? 1 : 0)
	    << "*kIORPCMessageLocalHost\n";
	out << "                             | " << (has_annotation(method.annotations, "reply") ? 1 : 0)
	    << "*kIORPCMessageOnqueue;\n";
	out << "    msg->content.__hdr.msgid = " << prefix << "_ID;\n";
	if (method.is_static) {
		out << "    msg->content.__object = (OSObjectRef) OSTypeID(" << klass.name << ");\n";
	} else if (target_param != nullptr) {
		out << "    msg->content.__object = (OSObjectRef) "
		    << target_param->name << ";\n";
	} else {
		out << "    msg->content.__object = (OSObjectRef) this;\n";
	}
	out << "    msg->content.__hdr.objectRefs = " << prefix << "_Msg_ObjRefs;\n";
	out << "    msg->mach.msgh_body.msgh_descriptor_count = "
	    << rpc_message_descriptor_count(classes, method) << ";\n\n";
	out << "    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;\n\n";
	for (const MethodInfo::ParamInfo &param : method.params) {
		write_impl_wrapper_input_assignment(out, classes, method, prefix, param);
	}
	if (!has_annotation(method.annotations, "reply")) {
		out << "#ifdef KERNEL\n";
		if (include_reply) {
			out << "    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl), .kernelContent = &buf.msg.content.__hdr };\n";
		} else {
			out << "    IORPC rpc = { .message = &buf.msg.mach, .reply = NULL, .sendSize = sizeof(*msg), .replySize = 0, .kernelContent = &buf.msg.content.__hdr };\n";
		}
		out << "#else /* KERNEL */\n";
		if (include_reply) {
			out << "    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };\n";
		} else {
			out << "    IORPC rpc = { .message = &buf.msg.mach, .reply = NULL, .sendSize = sizeof(*msg), .replySize = 0 };\n";
		}
		out << "#endif /* KERNEL */\n";
	}
	if (has_annotation(method.annotations, "reply")) {
		out << "\n";
		out << "    ret = kIOReturnSuccess;\n\n";
	} else if (method.is_static) {
		out << "    ret = OSMTypeID(" << klass.name << ")->Invoke(rpc);\n\n";
	} else if (target_param != nullptr) {
		out << "    ret = " << target_param->name << "->Invoke(rpc);\n\n";
	} else {
		out << "    if (supermethod) ret = supermethod((OSObject *)this, rpc);\n";
		out << "    else             ret = ((OSObject *)this)->Invoke(rpc);\n\n";
	}
	if (include_reply) {
		out << "    if (kIOReturnSuccess == ret)\n";
		out << "    do {\n";
		if (method_is_invoke_reply(method)) {
			out << "        if (rpl->mach.msgh.msgh_size < (sizeof(IORPCMessageMach) + sizeof(IORPCMessage))) { ret = kIOReturnIPCError; break; };\n";
			out << "        if (rpl->mach.msgh_body.msgh_descriptor_count >= 1)\n";
			out << "        {\n";
			out << "            if (rpl->mach.msgh.msgh_size < (sizeof(IORPCMessageMach) + sizeof(mach_msg_port_descriptor_t) + sizeof(IORPCMessage))) { ret = kIOReturnIPCError; break; };\n";
			out << "        }\n";
			out << "        else\n";
			out << "        {\n";
			out << "            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };\n";
			out << "            if (rpl->content.__hdr.msgid                  != " << prefix
			    << "_ID) { ret = kIOReturnIPCError; break; };\n";
			out << "            if (rpl->mach.msgh_body.msgh_descriptor_count != "
			    << rpc_reply_descriptor_count(classes, method)
			    << ") { ret = kIOReturnIPCError; break; };\n";
			out << "            if (" << prefix
			    << "_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };\n";
			out << "        }\n";
		} else {
			out << "        {\n";
			out << "            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };\n";
			out << "            if (rpl->content.__hdr.msgid                  != " << prefix
			    << "_ID) { ret = kIOReturnIPCError; break; };\n";
			out << "            if (rpl->mach.msgh_body.msgh_descriptor_count != "
			    << rpc_reply_descriptor_count(classes, method)
			    << ") { ret = kIOReturnIPCError; break; };\n";
			out << "            if (" << prefix
			    << "_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };\n";
			out << "        }\n";
		}
		out << "    }\n";
		out << "    while (false);\n";
		out << "    if (kIOReturnSuccess == ret)\n";
		out << "    {\n";
		if (method_is_invoke_reply(method)) {
			out << "        if (ret == kIOReturnSuccess) {\n";
			out << "            IORPCMessage * message;\n";
			out << "            OSObject     * object;\n\n";
			out << "#ifdef KERNEL\n";
			out << "            message = rpc.kernelContent;\n";
			out << "#else /* KERNEL */\n";
			out << "            message = IORPCMessageFromMach(rpc.reply, false);\n";
			out << "#endif /* KERNEL */\n";
			out << "            if ((rpc.reply->msgh_body.msgh_descriptor_count < 1) || !(kIORPCMessageOneway & message->flags)) {\n";
			out << "               ret = kIOReturnIPCError;\n";
			out << "            } else {\n";
			out << "              object  = (typeof(object)) message->objects[0];\n";
			out << "              if (!object) ret = kIOReturnIPCError;\n";
			out << "              else\n";
			out << "              {\n";
			out << "                  rpc.sendSize  = rpc.replySize;\n";
			out << "                  rpc.replySize = 0;\n";
			out << "                  rpc.reply     = NULL;\n\n";
			out << "                  ret = object->Invoke(rpc);\n";
			out << "              }\n";
			out << "          }\n";
			out << "        }\n";
		} else {
			for (const MethodInfo::ParamInfo &param : method.params) {
				write_impl_wrapper_output_copy(out, classes, method, param);
			}
		}
		out << "    }\n\n\n";
	}
	if (impl_wrapper_result_type(method) == "void") {
	} else {
		out << "    return (ret);\n";
	}
	out << "}\n\n";
}

void write_impl_create_action_body(std::ostringstream &out,
    const ClassInfo &klass, const MethodInfo &method)
{
	std::string target_method = method.type_method_name;
	std::string action_class = action_class_name(klass, method);
	out << "kern_return_t\n";
	out << klass.name << "::CreateAction" << method.name
	    << "(size_t referenceSize, OSAction ** action)\n";
	out << "{\n";
	out << "    kern_return_t ret;\n\n";
	out << "#if defined(IOKIT_ENABLE_SHARED_PTR)\n";
	out << "    OSSharedPtr<OSString>\n";
	out << "#else /* defined(IOKIT_ENABLE_SHARED_PTR) */\n";
	out << "    OSString *\n";
	out << "#endif /* !defined(IOKIT_ENABLE_SHARED_PTR) */\n";
	out << "    typeName = OSString::withCString(\"" << action_class << "\");\n";
	out << "    if (!typeName) {\n";
	out << "        return kIOReturnNoMemory;\n";
	out << "    }\n";
	out << "    ret = " << action_class << "::CreateWithTypeName(this,\n";
	out << "                           " << klass.name << "_" << method.name << "_ID,\n";
	out << "                           " << klass.name << "_" << target_method << "_ID,\n";
	out << "                           referenceSize,\n";
	out << "#if defined(IOKIT_ENABLE_SHARED_PTR)\n";
	out << "                           typeName.get(),\n";
	out << "#else /* defined(IOKIT_ENABLE_SHARED_PTR) */\n";
	out << "                           typeName,\n";
	out << "#endif /* !defined(IOKIT_ENABLE_SHARED_PTR) */\n";
	out << "                           action);\n\n";
	out << "#if !defined(IOKIT_ENABLE_SHARED_PTR)\n";
	out << "    typeName->release();\n";
	out << "#endif /* !defined(IOKIT_ENABLE_SHARED_PTR) */\n";
	out << "    return (ret);\n";
	out << "}\n\n";
}

std::string impl_invoke_argument(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method, const MethodInfo::ParamInfo &param)
{
	if (output_array_param_for_count(classes, method, param) != nullptr) {
		if (param_is_output(classes, param)) {
			return "&" + param.name;
		}
		return param.name;
	}
	if (input_array_param_for_count(classes, method, param) != nullptr) {
		return param.name;
	}
	if (param_is_output_array(classes, param)) {
		return "&reply->content.__" + param.name + "[0]";
	}
	if (param_is_inline_array(classes, param) && !param_is_output(classes, param)) {
		return "&MESSAGE_CONTENT(__" + param.name + "[0])";
	}
	if (param_is_object_array(classes, param)) {
		return param.name;
	}
	if (param_is_output(classes, param)) {
		if (param_is_serializable_object_pointer(classes, param)) {
			return "&reply->content." + param.name;
		}
		if (param_is_object_pointer(classes, param)) {
			return "(" + canonical_param_type(param) + ")&reply->content." + param.name;
		}
		return "&reply->content." + param.name;
	}
	if (param_is_const_struct_pointer(classes, param)) {
		return "&MESSAGE_CONTENT(" + param.name + ")";
	}
	if (param_is_const_value_pointer(param)) {
		return "&MESSAGE_CONTENT(" + param.name + ")";
	}
	if (param_is_serializable_object_pointer(classes, param)) {
		return "MESSAGE_CONTENT(" + param.name + ")";
	}
	if (param_is_object_pointer(classes, param)) {
		return param.name;
	}
	if (param_is_port_descriptor(param)) {
		return "message->" + param.name + "__descriptor.name";
	}
	if (param_is_inline_string(param)) {
		return "&MESSAGE_CONTENT(__" + param.name + "[0])";
	}
	if (param_is_unbounded_const_char_pointer(param)) {
		return "&MESSAGE_CONTENT(" + param.name + ")";
	}
	return "MESSAGE_CONTENT(" + param.name + ")";
}

void write_impl_invoke_body(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const MethodInfo &method,
    bool include_target_action_class)
{
	std::string prefix = rpc_prefix_for_method(klass, method);
	out << "kern_return_t\n";
	out << klass.name << "::" << method.name << "_Invoke(const IORPC _rpc,\n";
	if (!method.is_static) {
		out << "        OSMetaClassBase * target,\n";
	}
	out << "        " << method.name << "_Handler func";
	if (include_target_action_class) {
		out << ",\n";
		out << "        const OSMetaClass * targetActionClass";
	}
	out << ")\n";
	out << "{\n";
	out << "    " << prefix << "_Invocation rpc = { _rpc };\n";
	out << "    " << prefix << "_Rpl_With_Content * reply = rpc.reply;\n";
	out << "#ifdef KERNEL\n";
	out << "    const " << prefix << "_Msg *         message = rpc.message;\n";
	out << "    const " << prefix << "_Msg_Content * messageContent = rpc.kernelContent;\n";
	out << "#else /* KERNEL */\n";
	out << "    const " << prefix << "_Msg_With_Content * message = rpc.message;\n";
	out << "#endif /* KERNEL */\n";
	if (!method_is_oneway(method)) {
		out << "#if __has_builtin(__builtin_assume)\n";
		out << "    __builtin_assume(reply != NULL);\n";
		out << "#endif /* __has_builtin(__builtin_assume) */\n";
	}
	if (method.result_type != "void" && !has_annotation(method.annotations, "reply")) {
		out << "    kern_return_t ret;\n";
	}
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (param_is_object_array(classes, param)) {
			std::string object_class = object_class_name_for_param(classes, param);
			out << "#if !__LP64__\n";
			out << "    " << object_class
			    << " * " << param.name << "[" << param.array_count << "] = {};\n";
			out << "#else /* !__LP64__ */\n";
			out << "    " << object_class << " ** " << param.name << ";\n";
			out << "#endif /* __LP64__ */\n";
			continue;
		}
		if (!param_is_output(classes, param) &&
		    param_is_object_pointer(classes, param) &&
		    !param_is_object_array(classes, param) &&
		    !param_is_serializable_object_pointer(classes, param)) {
			out << "    " << object_class_name_for_param(classes, param)
			    << " * " << param.name << ";\n";
			continue;
		}
		if (const MethodInfo::ParamInfo *array_param =
		    input_array_param_for_count(classes, method, param)) {
			out << "    uint32_t " << param.name
			    << " = (sizeof(MESSAGE_CONTENT(__" << array_param->name
			    << ")) / sizeof(MESSAGE_CONTENT(__" << array_param->name
			    << "[0])));\n";
			out << "#pragma clang diagnostic push\n";
			out << "#pragma clang diagnostic ignored \"-Wtautological-unsigned-zero-compare\"\n";
			out << "    if (MESSAGE_CONTENT(" << param.name << ") >= 0 && "
			    << param.name << " > MESSAGE_CONTENT(" << param.name
			    << ")) " << param.name << " = MESSAGE_CONTENT("
			    << param.name << ");\n";
			out << "#pragma clang diagnostic pop\n";
			continue;
		}
		if (const MethodInfo::ParamInfo *array_param =
		    output_array_param_for_count(classes, method, param)) {
			if (param_is_output(classes, param)) {
				out << "    unsigned int " << param.name
				    << " = (sizeof(reply->content.__" << array_param->name
				    << ") / sizeof(reply->content.__" << array_param->name
				    << "[0]));\n";
			} else {
				out << "    " << param.type << " " << param.name
				    << " = (sizeof(MESSAGE_CONTENT(__" << array_param->name
				    << ")) / sizeof(MESSAGE_CONTENT(__" << array_param->name
				    << "[0])));\n";
			}
			out << "#pragma clang diagnostic push\n";
			out << "#pragma clang diagnostic ignored \"-Wtautological-unsigned-zero-compare\"\n";
			out << "    if (MESSAGE_CONTENT(" << param.name << ") >= 0 && "
			    << param.name << " > MESSAGE_CONTENT(" << param.name
			    << ")) " << param.name << " = MESSAGE_CONTENT("
			    << param.name << ");\n";
			out << "#pragma clang diagnostic pop\n";
			continue;
		}
	}
	out << "\n";
	out << "    if (" << rpc_message_descriptor_count(classes, method)
	    << " != message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);\n";
	out << "    if (" << prefix << "_Msg_ObjRefs != MESSAGE_CONTENT(__hdr.objectRefs)) return (kIOReturnIPCError);\n";
	out << "    if (rpc.sendSize < sizeof(" << prefix
	    << "_Msg_With_Content)) return (kIOReturnIPCError);\n";
	out << "    if (reply != NULL && rpc.replySize < sizeof(*reply)) return (kIOReturnIPCError);\n";
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (param_is_object_array(classes, param)) {
			std::string object_class = object_class_name_for_param(classes, param);
			if (param_is_output(classes, param)) {
				out << "#if __LP64__\n";
				out << "    " << param.name << " = (" << object_class
				    << " **)(uintptr_t) &reply->content.__" << param.name
				    << "[0];\n";
				out << "#endif /* __LP64__ */\n";
			} else {
				out << "#if !__LP64__\n";
				out << "    for (unsigned int idx = 0; idx < " << param.name
				    << "Count; idx++)\n";
				out << "    {\n";
				out << "        " << param.name << "[idx] = (" << object_class
				    << " *)(uintptr_t)MESSAGE_CONTENT(__" << param.name << "[idx]);\n";
				out << "    }\n";
				out << "#else /* !__LP64__ */\n";
				out << "    " << param.name << " = (" << object_class
				    << " **)(uintptr_t) &MESSAGE_CONTENT(__" << param.name << "[0]);\n";
				out << "#endif /* __LP64__ */\n";
				out << "    for (unsigned int idx = 0; idx < " << param.name
				    << "Count; idx++)\n";
				out << "    {\n";
				out << "        if (" << param.name << "[idx] && !OSDynamicCast("
				    << object_class << ", " << param.name
				    << "[idx])) return (kIOReturnBadArgument);\n";
				out << "    }\n";
			}
		}
	}
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (!param_is_output(classes, param) &&
		    param_is_serializable_object_pointer(classes, param)) {
			std::string object_class = object_class_name_for_param(classes, param);
			out << "    if (((OSObject *) MESSAGE_CONTENT(" << param.name
			    << ")) != NULL && OSDynamicCast(" << object_class
			    << ", (OSObject *) MESSAGE_CONTENT(" << param.name
			    << ")) == NULL) { return kIOReturnBadArgument; } \n";
		}
	}
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (!param_is_output(classes, param) &&
		    param_is_object_pointer(classes, param) &&
		    !param_is_object_array(classes, param) &&
		    !param_is_serializable_object_pointer(classes, param)) {
			std::string object_class = object_class_name_for_param(classes, param);
			if (include_target_action_class && object_class == "OSAction") {
				out << "    if (targetActionClass) {\n";
				out << "        " << param.name
				    << " = (OSAction *) OSMetaClassBase::safeMetaCast((OSObject *) MESSAGE_CONTENT("
				    << param.name << "), targetActionClass);\n";
				out << "    } else {\n";
				out << "        " << param.name << " = OSDynamicCast(" << object_class
				    << ", (OSObject *) MESSAGE_CONTENT(" << param.name << "));\n";
				out << "    }\n";
			} else {
				out << "    " << param.name << " = OSDynamicCast(" << object_class
				    << ", (OSObject *) MESSAGE_CONTENT(" << param.name << "));\n";
			}
			out << "    if (!" << param.name << " && MESSAGE_CONTENT(" << param.name
			    << ")) return (kIOReturnBadArgument);\n";
		}
	}
	for (const MethodInfo::ParamInfo &param : method.params) {
		if (!param_is_output(classes, param) && param_is_inline_string(param)) {
			out << "    if (strnlen(&MESSAGE_CONTENT(__" << param.name
			    << "[0]), sizeof(MESSAGE_CONTENT(__" << param.name
			    << "))) >= sizeof(MESSAGE_CONTENT(__" << param.name
			    << "))) return kIOReturnBadArgument;\n";
		}
	}
	out << "\n";
	if (method.result_type == "void" || has_annotation(method.annotations, "reply")) {
		out << "    (*func)(";
	} else {
		out << "    ret = (*func)(";
	}
	if (!method.is_static) {
		out << "target";
		if (method_is_invoke_reply(method) || !method.params.empty()) {
			out << ",\n";
		}
	}
	if (method_is_invoke_reply(method)) {
		out << "        rpc.rpc";
		if (!method.params.empty()) {
			out << ",";
			out << "\n";
		} else {
			out << ");\n\n";
		}
	}
	for (std::size_t i = 0; i < method.params.size(); ++i) {
		out << "        " << impl_invoke_argument(classes, method, method.params[i]);
		if (i + 1 != method.params.size()) {
			out << ",";
			out << "\n";
		} else {
			out << ");\n\n";
		}
	}
	if (method.params.empty()) {
		out << ");\n\n";
	}
	if (method.result_type == "void" || has_annotation(method.annotations, "reply")) {
		out << "\n";
		out << "    return (kIOReturnSuccess);\n";
		out << "}\n\n";
		return;
	}
	out << "    if (kIOReturnSuccess != ret) return (ret);\n\n";
	if (method_is_invoke_reply(method)) {
		out << "    IORPCMessage * replyMessage;\n\n";
		out << "#ifdef KERNEL\n";
		out << "    replyMessage = rpc.rpc.kernelContent;\n";
		out << "#else /* KERNEL */\n";
		out << "    replyMessage = IORPCMessageFromMach(rpc.rpc.reply, false);\n";
		out << "#endif /* KERNEL */\n";
		out << "    if ((rpc.rpc.reply->msgh_body.msgh_descriptor_count < 1)  || !(kIORPCMessageOneway & replyMessage->flags)) ret = kIOReturnIPCError;\n\n";
		out << "    return (ret);\n";
		out << "}\n\n";
		return;
	}
	if (!method_is_oneway(method)) {
		out << "    reply->content.__hdr.msgid = " << prefix << "_ID;\n";
		out << "    reply->content.__hdr.flags = kIORPCMessageOneway;\n";
		out << "    reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;\n";
		out << "    reply->mach.msgh.msgh_size = sizeof(*reply);\n";
		out << "    reply->mach.msgh_body.msgh_descriptor_count = "
		    << rpc_reply_descriptor_count(classes, method) << ";\n";
		out << "    reply->content.__hdr.objectRefs = " << prefix << "_Rpl_ObjRefs;\n";
		for (const MethodInfo::ParamInfo &param : method.params) {
			if (param_is_output(classes, param) &&
			    param_is_object_pointer(classes, param) &&
			    !param_is_serializable_object_pointer(classes, param)) {
				if (param_is_object_array(classes, param)) {
					out << "    for (unsigned int idx = 0; idx < "
					    << param.array_count << "; idx++)\n";
					out << "    {\n";
					out << "        reply->" << param.name
					    << "__descriptor[idx].type = MACH_MSG_PORT_DESCRIPTOR;\n";
					out << "#if !__LP64__\n";
					out << "        reply->content.__" << param.name
					    << "[idx] = (OSObjectRef)(uintptr_t) "
					    << param.name << "[idx];\n";
					out << "#endif /* !_LP64__ */\n";
					out << "    }\n";
				} else {
					out << "    reply->" << param.name
					    << "__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;\n";
				}
			}
		}
		for (const MethodInfo::ParamInfo &param : method.params) {
			if (output_array_param_for_count(classes, method, param) != nullptr &&
			    param_is_output(classes, param)) {
				out << "    reply->content." << param.name << " = "
				    << param.name << ";\n";
			}
		}
		for (const MethodInfo::ParamInfo &param : method.params) {
			if (param_is_output(classes, param) &&
			    param_is_serializable_object_pointer(classes, param)) {
				out << "    reply->" << param.name
				    << "__descriptor.type = MACH_MSG_OOL_DESCRIPTOR;\n";
				out << "    reply->" << param.name
				    << "__descriptor.copy = MACH_MSG_VIRTUAL_COPY;\n";
				out << "    reply->" << param.name
				    << "__descriptor.address = (void *) __builtin_offsetof("
				    << prefix << "_Rpl_Content, " << param.name << ");\n";
				out << "    reply->" << param.name
				    << "__descriptor.size = 0;\n";
			}
		}
		out << "\n";
	}
	out << "    return (ret);\n";
	out << "}\n\n";
}

void write_impl_method_bodies_for_class(std::ostringstream &out,
    const ParseState &state,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass)
{
	std::vector<const MethodInfo *> methods;
	for (const MethodInfo *method : rpc_methods_for_class(state, klass)) {
		if (method_has_type_annotation(*method) ||
		    method_overrides_rpc_root(classes, klass, *method)) {
			continue;
		}
		methods.push_back(method);
	}
	for (const MethodInfo *method : methods) {
		if (method_needs_impl_wrapper(klass, *method)) {
			write_impl_wrapper_signature(out, classes, klass, *method);
			write_impl_wrapper_body(out, classes, klass, *method);
		}
	}
	for (const MethodInfo *method : rpc_methods_for_class(state, klass)) {
		if (method_has_type_annotation(*method) &&
		    !method_overrides_rpc_root(classes, klass, *method)) {
			write_impl_create_action_body(out, klass, *method);
		}
	}
	for (const MethodInfo *method : methods) {
		bool action_target = !method->is_static &&
		    method_has_osaction_target_param(classes, *method);
		if (action_target) {
			out << "kern_return_t\n";
			out << klass.name << "::" << method->name
			    << "_Invoke(const IORPC _rpc,\n";
			out << "        OSMetaClassBase * target,\n";
			out << "        " << method->name << "_Handler func)\n";
			out << "{\n";
			out << "    return " << klass.name << "::" << method->name
			    << "_Invoke(_rpc, target, func, NULL);\n";
			out << "}\n\n";
		}
		write_impl_invoke_body(out, classes, klass, *method, action_target);
	}
}

void write_impl_regular_classes(std::ostringstream &out, const ParseState &state)
{
	std::map<std::string, const ClassInfo *> classes = class_map(state);
	std::vector<const ClassInfo *> impl_classes = generated_main_classes(state);
	if (impl_classes.empty()) {
		return;
	}
	write_impl_metaclass_declaration(out, state, *impl_classes.front());
	for (const ClassInfo *klass : impl_classes) {
		write_impl_class_description(out, state, classes, *klass);
		write_impl_dispatch(out, *klass, state, classes);
		write_impl_method_bodies_for_class(out, state, classes, *klass);
	}
}

void write_impl_action_class_description(std::ostringstream &out,
    const std::string &action_class)
{
	const char *availability =
	    "__attribute__((availability(driverkit,introduced=20,message=\"Type-safe OSAction factory methods are available in DriverKit 20 and newer\")))";

	out << "#if KERNEL\n";
	out << "OSDefineMetaClassAndStructors(" << action_class << ", OSAction);\n";
	out << "#endif /* KERNEL */\n\n";
	out << "#if !KERNEL\n\n";
	ImplClassDescription desc;
	desc.name = action_class;
	desc.super_name = "OSAction";
	desc.availability = availability;
	write_impl_class_description_records(out, desc);
	out << "#endif /* !KERNEL */\n\n";
}

void write_impl_action_dispatch(std::ostringstream &out,
    const std::string &action_class)
{
	out << "#ifdef KERNEL\n";
	out << "#define MESSAGE_CONTENT(__field) (messageContent->__field)\n";
	out << "#else /* KERNEL */\n";
	out << "#define MESSAGE_CONTENT(__field) (message->content.__field)\n";
	out << "#endif /* KERNEL */\n\n";
	out << "kern_return_t\n";
	out << action_class << "::Dispatch(const IORPC rpc)\n";
	out << "{\n";
	out << "    return _Dispatch(this, rpc);\n";
	out << "}\n\n";
	out << "kern_return_t\n";
	out << action_class << "::_Dispatch(" << action_class << " * self, const IORPC rpc)\n";
	out << "{\n";
	out << "    kern_return_t ret = kIOReturnUnsupported;\n";
	out << "#ifdef KERNEL\n";
	out << "    IORPCMessage * msg = rpc.kernelContent;\n";
	out << "#else /* KERNEL */\n";
	out << "    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);\n";
	out << "#endif /* KERNEL */\n\n";
	out << "    switch (msg->msgid)\n";
	out << "    {\n\n";
	out << "        default:\n";
	out << "            ret = OSAction::_Dispatch(self, rpc);\n";
	out << "            break;\n";
	out << "    }\n\n";
	out << "    return (ret);\n";
	out << "}\n\n";
	out << "#if KERNEL\n";
	out << "kern_return_t\n";
	out << action_class << "::MetaClass::Dispatch(const IORPC rpc)\n";
	out << "{\n";
	out << "#else /* KERNEL */\n";
	out << "kern_return_t\n";
	out << action_class << "MetaClass::Dispatch(const IORPC rpc)\n";
	out << "{\n";
	out << "#endif /* !KERNEL */\n\n";
	out << "    kern_return_t ret = kIOReturnUnsupported;\n";
	out << "#ifdef KERNEL\n";
	out << "    IORPCMessage * msg = rpc.kernelContent;\n";
	out << "#else /* KERNEL */\n";
	out << "    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);\n";
	out << "#endif /* KERNEL */\n\n";
	out << "    switch (msg->msgid)\n";
	out << "    {\n\n";
	out << "        default:\n";
	out << "            ret = OSMetaClassBase::Dispatch(rpc);\n";
	out << "            break;\n";
	out << "    }\n\n";
	out << "    return (ret);\n";
	out << "}\n\n";
}

void write_impl_action_class_blocks(std::ostringstream &out, const ParseState &state)
{
	for (const ClassInfo *klass : generated_main_classes(state)) {
		for (const std::string &action_class : action_class_names_for_type_methods(*klass)) {
			write_impl_action_class_description(out, action_class);
			write_impl_action_dispatch(out, action_class);
		}
	}
}

void write_class_replacement(std::ostringstream &out, const Options &opt,
    const ParseState &state,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass, const std::vector<std::string> &lines,
    unsigned source_start)
{
	out << "/* source class " << klass.name << " " << basename_of(opt.def)
	    << ":" << source_start << "-" << (klass.end_line - 1) << " */\n\n";
	out << "#if __DOCUMENTATION__\n"
	    << "#define KERNEL IIG_KERNEL\n\n";
	bool in_iig_block = false;
	for (unsigned line = source_start; line <= klass.end_line; ++line) {
		if (lines[line - 1].find("@iig implementation") != std::string::npos) {
			in_iig_block = true;
			continue;
		}
		if (lines[line - 1].find("@iig end") != std::string::npos) {
			in_iig_block = false;
			continue;
		}
		if (in_iig_block) {
			continue;
		}
		out << lines[line - 1] << "\n";
	}
	out << "\n#undef KERNEL\n"
	    << "#else /* __DOCUMENTATION__ */\n\n";
	write_generated_class(out, klass, state, classes, basename_of(opt.def),
	    source_start);
	write_action_classes_for_type_methods(out, klass);
	out << "#endif /* !__DOCUMENTATION__ */\n";
}

std::string generate_header(const Options &opt, const ParseState &state)
{
	std::vector<std::string> lines = read_lines(opt.def);
	std::ostringstream out;
	out << "/* iig(" << kIIGVersion << ") generated from "
	    << basename_of(opt.def) << " */\n\n";
	std::map<std::string, const ClassInfo *> classes = class_map(state);
	std::map<unsigned, const ClassInfo *> classes_by_start;
	std::map<unsigned, unsigned> extension_ranges;
	unsigned first_boundary = lines.empty() ? 0 : static_cast<unsigned>(lines.size());
	for (const ClassInfo &klass : state.classes) {
		if (klass.from_main_file && is_generated_header_class(klass) &&
		    annotation_value(klass.annotations, "extends=").empty()) {
			unsigned source_start = class_header_start_line(lines, klass);
			classes_by_start[source_start] = &klass;
			first_boundary = std::min(first_boundary, source_start);
		}
		if (klass.from_main_file &&
		    !annotation_value(klass.annotations, "extends=").empty()) {
			extension_ranges[klass.start_line] = klass.end_line;
		}
	}
	for (unsigned line_no = 1; line_no <= lines.size(); ++line_no) {
		if (trim_copy(lines[line_no - 1]) == "#if __IIG" &&
		    iig_only_block_contains_type_decl(lines, line_no)) {
			first_boundary = std::min(first_boundary, line_no);
			break;
		}
	}
	if (!lines.empty()) {
		if (classes_by_start.empty() && first_boundary == lines.size()) {
			out << "/* " << basename_of(opt.def) << ":1- */\n";
		} else {
			unsigned top_end = first_boundary > 1 ? first_boundary - 1 : 1;
			if (first_boundary > 0 && first_boundary <= lines.size() &&
			    trim_copy(lines[first_boundary - 1]) == "#if __IIG" &&
			    iig_only_block_contains_type_decl(lines, first_boundary)) {
				top_end = first_boundary;
			}
			out << "/* " << basename_of(opt.def) << ":1-" << top_end << " */\n";
		}
	}
	bool in_iig_block = false;
	bool in_iig_only_block = false;
	unsigned iig_only_depth = 0;
	bool pending_source_marker = false;
	bool pending_source_marker_leading_blank = true;
	bool pending_source_marker_extra_blank = false;
	for (unsigned line_no = 1; line_no <= lines.size(); ++line_no) {
		auto class_it = classes_by_start.find(line_no);
		if (class_it != classes_by_start.end()) {
			const ClassInfo *klass = class_it->second;
			write_class_replacement(out, opt, state, classes, *klass, lines,
			    line_no);
			line_no = klass->end_line;
			pending_source_marker = true;
			pending_source_marker_leading_blank = true;
			pending_source_marker_extra_blank = false;
			continue;
		}
		auto extension_it = extension_ranges.find(line_no);
		if (extension_it != extension_ranges.end()) {
			unsigned prev = line_no > 1 ? line_no - 1 : 0;
			while (prev > 0 && trim_copy(lines[prev - 1]).empty()) {
				--prev;
			}
			bool guarded_extension = prev > 0 &&
			    trim_copy(lines[prev - 1]).rfind("#if", 0) == 0;
			line_no = extension_it->second;
			pending_source_marker = true;
			pending_source_marker_leading_blank = !guarded_extension;
			pending_source_marker_extra_blank = !guarded_extension;
			continue;
		}
		const std::string &line = lines[line_no - 1];
		std::string trimmed = trim_copy(line);
		if (!in_iig_only_block && trimmed == "#if __IIG" &&
		    iig_only_block_contains_type_decl(lines, line_no)) {
			in_iig_only_block = true;
			iig_only_depth = 1;
			out << line << "\n";
			continue;
		}
		if (in_iig_only_block) {
			if (trimmed.rfind("#if", 0) == 0) {
				++iig_only_depth;
				continue;
			}
			if (trimmed.rfind("#endif", 0) == 0) {
				--iig_only_depth;
				if (iig_only_depth == 0) {
					out << "/* " << basename_of(opt.def) << ":"
					    << line_no << "-" << (line_no + 2) << " */\n";
					out << line << "\n";
					in_iig_only_block = false;
				}
				continue;
			}
			continue;
		}
		if (line.find("@iig implementation") != std::string::npos) {
			in_iig_block = true;
			continue;
		}
		if (line.find("@iig end") != std::string::npos) {
			in_iig_block = false;
			continue;
		}
		if (in_iig_block) {
			continue;
		}
		if (pending_source_marker) {
			if (trimmed.empty()) {
				unsigned next = line_no + 1;
				while (next <= lines.size() &&
				    trim_copy(lines[next - 1]).empty()) {
					++next;
				}
				if (next <= lines.size() &&
				    trim_copy(lines[next - 1]).rfind("#if", 0) == 0) {
					unsigned guarded = next + 1;
					while (guarded <= lines.size() &&
					    trim_copy(lines[guarded - 1]).empty()) {
						++guarded;
					}
					if (extension_ranges.find(guarded) != extension_ranges.end()) {
						out << "\n";
						out << "/* " << basename_of(opt.def) << ":" << line_no
						    << "-" << next << " */\n";
						pending_source_marker = false;
						pending_source_marker_leading_blank = true;
						pending_source_marker_extra_blank = false;
					}
				}
				if (extension_ranges.find(next) != extension_ranges.end()) {
					if (pending_source_marker_extra_blank) {
						out << "\n";
					}
					continue;
				}
				if (classes_by_start.find(next) != classes_by_start.end()) {
					out << "\n\n";
					if (pending_source_marker_extra_blank) {
						out << "\n";
					}
					continue;
				}
			}
			if (pending_source_marker) {
				if (pending_source_marker_leading_blank) {
					out << "\n";
				}
				if (pending_source_marker_extra_blank) {
					out << "\n";
				}
				unsigned source_end = 0;
				for (unsigned next = line_no + 1; next <= lines.size(); ++next) {
					if (classes_by_start.find(next) != classes_by_start.end() ||
					    extension_ranges.find(next) != extension_ranges.end()) {
						source_end = next - 1;
						break;
					}
				}
				out << "/* " << basename_of(opt.def) << ":" << line_no << "-";
				if (source_end != 0) {
					out << source_end;
				}
				out << " */\n";
				pending_source_marker = false;
				pending_source_marker_leading_blank = true;
				pending_source_marker_extra_blank = false;
			}
		}
		out << rewrite_include_line(line) << "\n";
	}

	return out.str();
}

std::string implementation_block(const Options &opt, const ParseState &state)
{
	std::vector<std::string> lines = read_lines(opt.def);
	std::ostringstream out;
	for (const ClassInfo *klass : generated_main_classes(state)) {
		unsigned start = class_header_start_line(lines, *klass);
		if (start == 0 || start > lines.size()) {
			continue;
		}
		bool in_block = false;
		for (unsigned line_no = start;
		    line_no <= klass->end_line && line_no <= lines.size(); ++line_no) {
			const std::string &line = lines[line_no - 1];
			if (line.find("@iig implementation") != std::string::npos) {
				in_block = true;
				out << "/* @iig implementation */\n";
				continue;
			}
			if (line.find("@iig end") != std::string::npos) {
				if (in_block) {
					out << "/* @iig end */\n";
				}
				in_block = false;
				continue;
			}
			if (in_block) {
				out << rewrite_include_line(line) << "\n";
			}
		}
		if (in_block) {
			out << "/* @iig end */\n";
		}
	}
	return out.str();
}

std::string generate_impl(const Options &opt, const ParseState &state)
{
	std::ostringstream out;
	std::vector<const ClassInfo *> classes = generated_main_classes(state);
	out << "/* iig(" << kIIGBannerVersion << ") generated from "
	    << basename_of(opt.def) << " */\n\n";
	out << "#undef\tIIG_IMPLEMENTATION\n";
	out << "#define\tIIG_IMPLEMENTATION \t" << basename_of(opt.def) << "\n\n";
	if (!opt.framework_name.empty()) {
		out << "#if KERNEL\n"
		    << "#include <libkern/c++/OSString.h>\n"
		    << "#else\n"
		    << "#include <" << opt.framework_name << "/" << opt.framework_name << ".h>\n"
		    << "#endif /* KERNEL */\n"
		    << "#include <" << opt.framework_name << "/IOReturn.h>\n";
		for (const ClassInfo *klass : classes) {
			out << "#include <" << opt.framework_name << "/" << klass->name
			    << ".h>\n";
		}
		out << "\n";
	}
	std::string iig_impl = implementation_block(opt, state);
	if (!iig_impl.empty()) {
		out << iig_impl << "\n";
	}
	out << "\n"
	    << "#if __has_builtin(__builtin_load_member_function_pointer)\n"
	    << "#define SimpleMemberFunctionCast(cfnty, self, func) (cfnty)__builtin_load_member_function_pointer(self, func)\n"
	    << "#else\n"
	    << "#define SimpleMemberFunctionCast(cfnty, self, func) ({ union { typeof(func) memfun; cfnty cfun; } pair; pair.memfun = func; pair.cfun; })\n"
	    << "#endif\n";
	if (classes.empty()) {
		out << "\n";
		return out.str();
	}
	out << "\n\n";
	write_impl_rpc_types(out, state);
	write_impl_regular_classes(out, state);
	write_impl_action_class_blocks(out, state);
	out << "\n\n";
	return out.str();
}

std::string edit_signature(const std::map<std::string, const ClassInfo *> &classes,
    const MethodInfo &method, bool include_invokereply)
{
	std::ostringstream sig;
	if (include_invokereply) {
		sig << "const IORPC rpc";
		if (!method.params.empty()) {
			sig << ",\\\n        ";
		}
	}
	for (std::size_t i = 0; i < method.params.size(); ++i) {
		if (i != 0) {
			sig << ",\\\n        ";
		}
		sig << declaration_param_type(classes, method.params[i]);
		if (!method.params[i].name.empty()) {
			sig << " " << method.params[i].name;
		}
	}
	return sig.str();
}

void write_edits_for_class(std::ostringstream &out,
    const std::map<std::string, const ClassInfo *> &classes,
    const ClassInfo &klass)
{
	bool kernel_class = has_annotation(klass.annotations, "kernel");
	if (!klass.from_main_file) {
		return;
	}
	std::string edit_class = annotation_value(klass.annotations, "extends=");
	if (edit_class.empty()) {
		edit_class = klass.name;
	}
	for (const MethodInfo &method : klass.methods) {
		bool emit = has_annotation(method.annotations, "kernel");
		if (kernel_class && !has_annotation(method.annotations, "localonly")) {
			emit = true;
		}
		if (!annotation_value(klass.annotations, "extends=").empty() &&
		    !has_annotation(method.annotations, "localonly")) {
			emit = true;
		}
		if (kernel_class && method_is_localonly(classes, klass, method) &&
		    !has_annotation(method.annotations, "kernel")) {
			emit = false;
		}
		if (!emit) {
			continue;
		}
		out << "s/IMPL(" << edit_class << ", " << method.name << ")/"
		    << edit_class << "::" << method.name << "_Impl(";
		if (method.params.empty()) {
			out << ")/\n";
		} else {
			bool include_invokereply = has_annotation(method.annotations, "invokereply") ||
			    class_or_base_method_has_annotation(classes, klass.base,
			    method.name, "invokereply");
			out << "\\\n        " << edit_signature(classes, method,
			    include_invokereply) << ")/\n";
		}
	}
}

std::string generate_edits(const ParseState &state)
{
	std::ostringstream out;
	std::map<std::string, const ClassInfo *> classes = class_map(state);
	for (auto it = state.classes.rbegin(); it != state.classes.rend(); ++it) {
		if (!annotation_value(it->annotations, "extends=").empty()) {
			write_edits_for_class(out, classes, *it);
		}
	}
	for (const ClassInfo &klass : state.classes) {
		if (annotation_value(klass.annotations, "extends=").empty()) {
			write_edits_for_class(out, classes, klass);
		}
	}
	return out.str();
}

std::string generate_log(const ParseState &state)
{
	std::ostringstream out;
	for (const ClassInfo &klass : state.classes) {
		out << "class " << klass.name;
		if (!klass.base.empty()) {
			out << " base=" << klass.base;
		}
		out << " lines=" << klass.start_line << "-" << klass.end_line;
		for (const std::string &ann : klass.annotations) {
			out << " annotation=" << ann;
		}
		out << "\n";
		for (const MethodInfo &method : klass.methods) {
			out << "method " << klass.name << "::" << method.name
			    << " result=" << method.result_type
			    << " lines=" << method.start_line << "-" << method.end_line;
			for (const std::string &ann : method.annotations) {
				out << " annotation=" << ann;
			}
			out << "\n";
			for (const MethodInfo::ParamInfo &param : method.params) {
				out << "param " << klass.name << "::" << method.name
				    << " type=" << param.type
				    << " name=" << param.name;
				for (const std::string &ann : param.annotations) {
					out << " annotation=" << ann;
				}
				out << "\n";
			}
		}
	}
	return out.str();
}

void write_trace(const ParseState &state)
{
	const char *trace_path = std::getenv("IIG_TRACE_HEADERS");
	if (!trace_path || trace_path[0] == '\0') {
		return;
	}
	std::ofstream out(trace_path, std::ios::app);
	if (!out) {
		std::fprintf(stderr, "cannot write %s: %s\n", trace_path, std::strerror(errno));
		return;
	}
	auto it = state.includes.find(state.main_file);
	out << "{\"file\":\"" << json_escape(state.main_file) << "\",\"includes\":[";
	if (it != state.includes.end()) {
		bool first = true;
		for (const std::string &include : it->second) {
			if (!first) {
				out << ",";
			}
			first = false;
			out << "\"" << json_escape(include) << "\"";
		}
	}
	out << "]}\n";
}

void resolve_parameter_facts(ParseState *state)
{
	std::map<std::string, const ClassInfo *> classes = class_map(*state);
	for (ClassInfo &klass : state->classes) {
		for (MethodInfo &method : klass.methods) {
			for (MethodInfo::ParamInfo &param : method.params) {
				param.object_class_name = object_class_name_for_param(classes,
				    param);
				auto it = classes.find(param.object_class_name);
				param.object_class_is_serializable = it != classes.end() &&
				    has_annotation(it->second->annotations, "serializable");
				param.pointee_is_struct = type_is_struct(classes,
				    pointee_type(param.canonical_type.empty() ?
				    param.type : param.canonical_type));
				param.class_facts_resolved = true;
			}
			for (MethodInfo::ParamInfo &array_param : method.params) {
				if (array_param.array_count.empty() ||
				    array_param.name.empty()) {
					continue;
				}
				const std::string count_name = array_param.name + "Count";
				for (MethodInfo::ParamInfo &count_param : method.params) {
					if (count_param.name != count_name) {
						continue;
					}
					count_param.associated_array_param_name = array_param.name;
					array_param.associated_count_param_name = count_param.name;
				}
			}
		}
	}
}

} // namespace

int main(int argc, char **argv)
{
	Options opt = parse_args(argc, argv);
	if (opt.show_help) {
		usage(stdout, argv[0]);
		return 0;
	}
	if (opt.show_version) {
		std::puts(kIIGVersion);
		return 0;
	}
	if (opt.def.empty() || opt.header.empty() || opt.impl.empty()) {
		usage(stderr, argv[0]);
		return 1;
	}

	ParseState state;
	if (parse_iig(opt, &state) != 0) {
		return 1;
	}

	bool ok = true;
	ok = write_file(opt.header, generate_header(opt, state)) && ok;
	ok = write_file(opt.impl, generate_impl(opt, state)) && ok;
	if (!opt.edits.empty()) {
		ok = write_file(opt.edits, generate_edits(state)) && ok;
	}
	if (!opt.log.empty()) {
		ok = write_file(opt.log, generate_log(state)) && ok;
	}
	write_trace(state);
	return ok ? 0 : 1;
}
