#!/usr/bin/env python3
"""
Parses LuaBridge binding files to generate engine_api_suggestions.h
This is invoked at build time by CMake.

It scans engine/core/script/binding/*.cpp for:
  - beginNamespace / addVariable  → enums
  - beginClass / deriveClass      → classes
  - addConstructor                → constructors
  - addFunction / addStaticFunction → methods
  - addProperty / addStaticProperty → properties
"""

import re
import sys
import os
import glob

# ── Regex patterns ──────────────────────────────────────────────────────────

RE_NAMESPACE_BEGIN = re.compile(r'\.beginNamespace\(\s*"([^"]+)"\s*\)')
RE_NAMESPACE_END = re.compile(r'\.endNamespace\(\)')
RE_ADD_VARIABLE = re.compile(r'\.addVariable\(\s*"([^"]+)"')

RE_BEGIN_CLASS = re.compile(
    r'\.beginClass\s*<\s*([^>]+?)\s*>\s*\(\s*"([^"]+)"\s*\)'
)
RE_DERIVE_CLASS = re.compile(
    r'\.deriveClass\s*<\s*([^,>]+?)\s*,\s*([^>]+?)\s*>\s*\(\s*"([^"]+)"\s*\)'
)
RE_END_CLASS = re.compile(r'\.endClass\(\)')

RE_ADD_CONSTRUCTOR = re.compile(r'\.addConstructor\s*<([^>]+)>')

RE_ADD_FUNCTION = re.compile(r'\.addFunction\(\s*"([^"]+)"')
RE_ADD_STATIC_FUNCTION = re.compile(r'\.addStaticFunction\(\s*"([^"]+)"')

RE_ADD_PROPERTY = re.compile(r'\.addProperty\(\s*"([^"]+)"')
RE_ADD_STATIC_PROPERTY = re.compile(r'\.addStaticProperty\(\s*"([^"]+)"')


class APISymbol:
    """Represents one symbol in the engine API."""
    __slots__ = ('name', 'kind', 'detail', 'parent')

    def __init__(self, name, kind, detail='', parent=''):
        self.name = name
        self.kind = kind
        self.detail = detail
        self.parent = parent

    def __repr__(self):
        return f'APISymbol({self.name!r}, {self.kind!r})'


def parse_binding_file(filepath):
    """Parse a single LuaBridge binding .cpp file."""
    symbols = []
    with open(filepath, 'r') as f:
        content = f.read()

    # Remove C++ comments
    content = re.sub(r'//.*', '', content)
    content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)

    # ── Parse enums (namespace-based) ───────────────────────────────────
    ns_pattern = re.compile(
        r'\.beginNamespace\(\s*"([^"]+)"\s*\)(.*?)\.endNamespace\(\)',
        re.DOTALL
    )
    for m in ns_pattern.finditer(content):
        ns_name = m.group(1)
        ns_body = m.group(2)

        # Skip internal FunctionSubscribe namespaces
        if 'FunctionSubscribe' in ns_name:
            continue

        values = RE_ADD_VARIABLE.findall(ns_body)
        if values:
            symbols.append(APISymbol(ns_name, 'Enum', f'enum {ns_name}'))
            for val_name in values:
                symbols.append(APISymbol(
                    val_name, 'EnumMember', f'{ns_name}.{val_name}', ns_name
                ))

    # ── Parse classes ───────────────────────────────────────────────────
    # We process the file line-by-line with state tracking
    lines = content.split('\n')
    current_class = None
    current_lua_name = None
    current_base = None
    has_constructor = False

    for line in lines:
        stripped = line.strip()

        # beginClass
        m = RE_BEGIN_CLASS.search(stripped)
        if m:
            current_class = m.group(1).strip()
            current_lua_name = m.group(2)
            current_base = None
            has_constructor = False
            continue

        # deriveClass
        m = RE_DERIVE_CLASS.search(stripped)
        if m:
            current_class = m.group(1).strip()
            current_base = m.group(2).strip()
            current_lua_name = m.group(3)
            has_constructor = False
            continue

        # endClass
        if RE_END_CLASS.search(stripped):
            if current_lua_name:
                base_info = f' : {current_base}' if current_base else ''
                detail = f'class {current_lua_name}{base_info}'
                symbols.append(APISymbol(current_lua_name, 'Class', detail))
                if has_constructor:
                    symbols.append(APISymbol(
                        current_lua_name, 'Constructor',
                        f'{current_lua_name}()', current_lua_name
                    ))
            current_class = None
            current_lua_name = None
            current_base = None
            continue

        if not current_lua_name:
            continue

        # Constructor
        if RE_ADD_CONSTRUCTOR.search(stripped):
            has_constructor = True
            continue

        # Static function
        m = RE_ADD_STATIC_FUNCTION.search(stripped)
        if m:
            fname = m.group(1)
            if not fname.startswith('__'):
                symbols.append(APISymbol(
                    fname, 'StaticMethod',
                    f'{current_lua_name}.{fname}()', current_lua_name
                ))
            continue

        # Member function
        m = RE_ADD_FUNCTION.search(stripped)
        if m:
            fname = m.group(1)
            if not fname.startswith('__'):
                symbols.append(APISymbol(
                    fname, 'Method',
                    f'{current_lua_name}:{fname}()', current_lua_name
                ))
            continue

        # Static property
        m = RE_ADD_STATIC_PROPERTY.search(stripped)
        if m:
            pname = m.group(1)
            if not pname.startswith('__'):
                # Check if it's an event callback (lambda pattern)
                if '[]' in stripped or 'lua_State' in stripped:
                    symbols.append(APISymbol(
                        pname, 'Event',
                        f'{current_lua_name}.{pname}', current_lua_name
                    ))
                else:
                    symbols.append(APISymbol(
                        pname, 'Constant',
                        f'{current_lua_name}.{pname}', current_lua_name
                    ))
            continue

        # Member property
        m = RE_ADD_PROPERTY.search(stripped)
        if m:
            pname = m.group(1)
            if not pname.startswith('__'):
                symbols.append(APISymbol(
                    pname, 'Property',
                    f'{current_lua_name}.{pname}', current_lua_name
                ))
            continue

    return symbols


def kind_to_suggestion_kind(kind):
    """Map our internal kind to SuggestionKind enum."""
    mapping = {
        'Class': 'Class',
        'Constructor': 'Class',
        'Enum': 'Enum',
        'EnumMember': 'EnumMember',
        'Method': 'Method',
        'StaticMethod': 'Function',
        'Property': 'Property',
        'Constant': 'Constant',
        'Event': 'Property',
    }
    return mapping.get(kind, 'Variable')


def _extract_class_body(text, start):
    """Extract a brace-delimited class body starting from the '{' at position start."""
    depth = 0
    i = start
    while i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
        i += 1
    return ''


def _simplify_param(param):
    """Simplify a C++ parameter to 'Type name' form, stripping const/ref/ptr qualifiers."""
    p = param.strip()
    if not p:
        return ''
    # Remove 'const' prefix
    p = re.sub(r'\bconst\s+', '', p)
    # Remove trailing const
    p = re.sub(r'\s+const$', '', p)
    # Remove std:: prefix
    p = re.sub(r'\bstd::', '', p)
    # Remove reference/pointer from type: 'string&' -> 'string'
    p = re.sub(r'\s*[&*]+\s*', ' ', p)
    # Collapse whitespace
    p = re.sub(r'\s+', ' ', p).strip()
    return p


def _simplify_params(params_str):
    """Simplify a full parameter list string."""
    if not params_str.strip():
        return ''
    params = params_str.split(',')
    simplified = []
    for p in params:
        s = _simplify_param(p)
        if s:
            simplified.append(s)
    return ', '.join(simplified)


def parse_cpp_headers(base_dir):
    """Parse C++ headers to extract public method declarations with parameter info."""
    # class_methods[class_name] = { method_name: [(params_str, return_type), ...] }
    class_methods = {}
    sig_pattern = re.compile(
        r'(?:virtual\s+|inline\s+|static\s+)*'
        r'([A-Za-z0-9_:<>&*\s]+)\s+'
        r'([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*(?:const)?\s*(?:override|final)?\s*[;{]'
    )
    class_start = re.compile(
        r'(?:class|struct)\s+(?:[A-Z0-9_]+\s+)?([A-Za-z0-9_]+)\s*(?::[^{]+)?\{'
    )

    for root, dirs, files in os.walk(base_dir):
        for f in files:
            if not f.endswith('.h'):
                continue
            path = os.path.join(root, f)
            with open(path, 'r', encoding='utf-8', errors='ignore') as fh:
                text = fh.read()

            # Strip comments
            text = re.sub(r'//.*', '', text)
            text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)

            for cm in class_start.finditer(text):
                class_name = cm.group(1)
                brace_pos = cm.end() - 1  # position of '{'
                body = _extract_class_body(text, brace_pos)
                if not body:
                    continue

                # Find the public section
                public_idx = body.find('public:')
                if public_idx == -1:
                    continue
                protected_idx = body.find('protected:', public_idx + 1)
                private_idx = body.find('private:', public_idx + 1)

                end_idx = len(body)
                if protected_idx != -1 and protected_idx < end_idx:
                    end_idx = protected_idx
                if private_idx != -1 and private_idx < end_idx:
                    end_idx = private_idx

                public_body = body[public_idx:end_idx]

                if class_name not in class_methods:
                    class_methods[class_name] = {}

                for m in sig_pattern.finditer(public_body):
                    ret_type = m.group(1).strip()
                    method_name = m.group(2)
                    params_raw = m.group(3).strip()
                    if method_name == class_name or method_name.startswith('~'):
                        continue
                    params = _simplify_params(params_raw)
                    if method_name not in class_methods[class_name]:
                        class_methods[class_name][method_name] = []
                    class_methods[class_name][method_name].append(params)

    return class_methods

def generate_header(symbols, output_path):
    """Generate engine_api_suggestions.h from parsed symbols."""
    # Deduplicate: keep first occurrence per (name, kind, parent) tuple
    seen = set()
    unique = []
    for s in symbols:
        key = (s.name, s.kind, s.parent)
        if key not in seen:
            seen.add(key)
            unique.append(s)

    # Separate into categories for organized output
    enums = [s for s in unique if s.kind == 'Enum']
    enum_members = [s for s in unique if s.kind == 'EnumMember']
    classes = [s for s in unique if s.kind == 'Class']
    constructors = [s for s in unique if s.kind == 'Constructor']
    methods = [s for s in unique if s.kind == 'Method']
    static_methods = [s for s in unique if s.kind == 'StaticMethod']
    properties = [s for s in unique if s.kind == 'Property']
    constants = [s for s in unique if s.kind == 'Constant']
    events = [s for s in unique if s.kind == 'Event']

    def escape(s):
        return s.replace('\\', '\\\\').replace('"', '\\"')

    lines = []
    lines.append('// Auto-generated by generate_api_suggestions.py — DO NOT EDIT')
    lines.append('// Parsed from engine/core/script/binding/*.cpp')
    lines.append('#pragma once')
    lines.append('')
    lines.append('#include <string>')
    lines.append('#include <vector>')
    lines.append('#include <unordered_set>')
    lines.append('')
    lines.append('namespace doriax::editor {')
    lines.append('')

    # ── Engine type names (for syntax highlighting in both Lua and C++) ──
    lines.append('inline std::unordered_set<std::string> getEngineTypeNames() {')
    lines.append('    return {')
    class_names = sorted(set(s.name for s in classes))
    for name in class_names:
        lines.append(f'        "{escape(name)}",')
    lines.append('    };')
    lines.append('}')
    lines.append('')

    # ── Engine enum names ────────────────────────────────────────────────
    lines.append('inline std::unordered_set<std::string> getEngineEnumNames() {')
    lines.append('    return {')
    enum_names = sorted(set(s.name for s in enums))
    for name in enum_names:
        lines.append(f'        "{escape(name)}",')
    lines.append('    };')
    lines.append('}')
    lines.append('')

    # ── Engine builtin function names (static classes used as modules) ──
    lines.append('inline std::unordered_set<std::string> getEngineBuiltinNames() {')
    lines.append('    return {')
    # Classes that are used as static modules (all static functions, no constructor)
    static_classes = set()
    for s in static_methods:
        static_classes.add(s.parent)
    # Also add enum names as builtins for Lua namespace access
    all_builtins = sorted(static_classes | set(enum_names))
    for name in all_builtins:
        lines.append(f'        "{escape(name)}",')
    lines.append('    };')
    lines.append('}')
    lines.append('')

    # ── Full suggestion symbols ──────────────────────────────────────────
    lines.append('struct EngineAPISymbol {')
    lines.append('    const char* name;')
    lines.append('    const char* kind;   // maps to SuggestionKind')
    lines.append('    const char* detail;')
    lines.append('    const char* parent; // owning class/enum')
    lines.append('};')
    lines.append('')
    lines.append('inline const std::vector<EngineAPISymbol>& getEngineAPISymbols() {')
    lines.append('    static const std::vector<EngineAPISymbol> symbols = {')

    all_symbols = enums + enum_members + classes + methods + static_methods + \
                  properties + constants + events
    for s in all_symbols:
        sk = kind_to_suggestion_kind(s.kind)
        lines.append(
            f'        {{"{escape(s.name)}", "{sk}", '
            f'"{escape(s.detail)}", "{escape(s.parent)}"}},')

    lines.append('    };')
    lines.append('    return symbols;')
    lines.append('}')
    lines.append('')
    lines.append('} // namespace doriax::editor')
    lines.append('')

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))

    print(f'[generate_api_suggestions] Generated {output_path}')
    print(f'  Classes: {len(classes)}, Methods: {len(methods)}, '
          f'StaticMethods: {len(static_methods)}, Properties: {len(properties)}, '
          f'Constants: {len(constants)}, Events: {len(events)}, '
          f'Enums: {len(enums)}, EnumMembers: {len(enum_members)}')


def main():
    if len(sys.argv) < 3:
        print(f'Usage: {sys.argv[0]} <binding_dir> <output_header>')
        sys.exit(1)

    if len(sys.argv) < 3:
        print(f'Usage: {sys.argv[0]} <binding_dir> <output_header> [engine_core_dir]')
        sys.exit(1)

    binding_dir = sys.argv[1]
    output_path = sys.argv[2]
    engine_core_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.abspath(os.path.join(binding_dir, '..', '..'))

    binding_files = sorted(glob.glob(os.path.join(binding_dir, '*.cpp')))
    if not binding_files:
        print(f'WARNING: No binding files found in {binding_dir}')
        sys.exit(1)

    all_symbols = []
    for filepath in binding_files:
        syms = parse_binding_file(filepath)
        all_symbols.extend(syms)
        print(f'  Parsed {os.path.basename(filepath)}: {len(syms)} symbols')

    cpp_methods = parse_cpp_headers(engine_core_dir)

    # Build a lookup of all method param signatures: (class, method) -> params_str
    cpp_signatures = {}  # (class_name, method_name) -> shortest params string
    for cls, methods_dict in cpp_methods.items():
        for method_name, overloads in methods_dict.items():
            # Pick the shortest overload as primary detail (most common / simplest)
            shortest = min(overloads, key=len) if overloads else ''
            cpp_signatures[(cls, method_name)] = shortest

    # Update existing LuaBridge method symbols with param info from C++ headers
    for s in all_symbols:
        if s.kind in ('Method', 'StaticMethod') and s.parent:
            sig = cpp_signatures.get((s.parent, s.name))
            if sig is not None:
                sep = '.' if s.kind == 'StaticMethod' else ':'
                s.detail = f'{s.parent}{sep}{s.name}({sig})'

    # Find all classes from LuaBridge and append C++ methods not already present
    lua_classes = {s.name for s in all_symbols if s.kind == 'Class'}
    existing_methods = {(s.name, s.parent) for s in all_symbols if s.kind == 'Method'}
    cpp_symbols = []
    for cls in lua_classes:
        methods_dict = cpp_methods.get(cls, {})
        for method_name, overloads in methods_dict.items():
            if (method_name, cls) not in existing_methods:
                shortest = min(overloads, key=len) if overloads else ''
                cpp_symbols.append(APISymbol(
                    method_name, 'Method',
                    f'{cls}:{method_name}({shortest})', cls
                ))
    all_symbols.extend(cpp_symbols)

    generate_header(all_symbols, output_path)


if __name__ == '__main__':
    main()
