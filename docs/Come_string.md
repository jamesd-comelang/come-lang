# 📚 Come String Module Reference (vs. C and Go)

This table defines the standard string methods available on the Come `string` object, designed to be instantly recognizable by C programmers while incorporating modern features like object-method style and UTF-8 awareness.

## Memory Model
Methods that create new strings (like `upper()`, `repeat()`, `replace()`) allocate the new string using the **parent string** as the memory context. This ensures hierarchical memory management: freeing the parent string automatically frees all derived temporary strings.

Any object in Come has a default method `a.chown(b)`, which changes the memory context of `a` to `b`'s context. If a derived string needs to outlive its parent, use `new_str.chown(new_parent)` to move it.

| Come Method | Description | C Equivalent | Go Equivalent |
| :--- | :--- | :--- | :--- |
| **a.size()** | Returns the number of **bytes** in the string. | *None* | `len(a)` |
| **a.len()** | Returns the number of **characters**  in the string. | `strlen(a)` | `utf8.RuneCountInString(a)` |
| **a.cmp(b[, n])** | Compares string `a` and `b` lexicographically. If `n` is provided, compares up to the first `n` UTF-8 characters; otherwise compares the entire strings. Returns 0 if equal, < 0 if `a < b`, > 0 if `a > b`. | `strcmp(a, b)` / `strncmp(a, b, n)` | `strings.Compare(a, b)` (full string), slice `a[:n]` for partial comparison |
| **a.casecmp(b[, n])** | Case-insensitively compares string `a` and `b`. If `n` is provided, compares up to the first `n` UTF-8 characters. Returns 0 if equal ignoring case, < 0 if `a < b`, > 0 if `a > b`. | `strcasecmp(a, b)` / `strncasecmp(a, b, n)` | `strings.EqualFold(a, b)` (for equality), use slice for first `n` characters |
| **a.chr(c)** | Finds the first occurrence of **character** `c` in the string. Returns index or $-1$. | `strchr(a, c)` | `strings.IndexByte(a, c)` |
| **a.rchr(c)** | Finds the last occurrence of **character** `c` in the string. Returns index or $-1$. | `strrchr(a, c)` | `strings.LastIndexByte(a, c)` |
| **a.memchr(c, n)** | Finds the first occurrence of **character** `c` in the first `n` characters of the string. | `memchr(a, c, n)` | `bytes.IndexByte(a[:n], c)` |
| **a.find(sub)** | Finds the first occurrence of substring `sub` in the string. Returns character index or $-1$. | `strstr(a, sub)` | `strings.Index(a, sub)` |
| **a.rfind(sub)** | Finds the last occurrence of substring `sub` in the string. Returns character index or $-1$. | `strrstr(a, sub)` | `strings.LastIndex(a, sub)` |
| **a.count(sub)** | Returns the number of non-overlapping occurrences of substring `sub`. | *None* | `strings.Count(a, sub)` |
| **a.upper()** | Returns a copy with all characters converted to uppercase. | `toupper()` (per-char) | `strings.ToUpper(a)` |
| **a.lower()** | Returns a copy with all characters converted to lowercase. | `tolower()` (per-char) | `strings.ToLower(a)` |
| **a.isdigit()** | Returns `true` if all characters are decimal digits. | `isdigit()` (per-char) | `unicode.IsDigit(r)` (per-rune) |
| **a.isalpha()** | Returns `true` if all characters are alphabetic. | `isalpha()` (per-char) | `unicode.IsLetter(r)` (per-rune) |
| **a.isalnum()** | Returns `true` if all characters are alphanumeric. | `isalnum()` (per-char) | `unicode.IsLetter(r) || unicode.IsDigit(r)` (per-rune) |
| **a.isspace()** | Returns `true` if all characters are whitespace. | `isspace()` (per-char) | `unicode.IsSpace(r)` (per-rune) |
| **a.utf8()** | Returns `true` if the string is valid UTF-8. | *None* | `utf8.ValidString(a)` |
| **a.trim([cutset])** | Removes leading and trailing Unicode whitespace characters if `cutset` is omitted; otherwise removes leading and trailing characters contained in `cutset`. | *None* | `strings.TrimSpace` / `strings.Trim` |
| **a.ltrim([cutset])** | Removes leading Unicode whitespace characters if `cutset` is omitted; otherwise removes leading characters contained in `cutset`. | *None* | `strings.TrimLeftFunc` / `strings.TrimLeft` |
| **a.rtrim([cutset])** | Removes trailing Unicode whitespace characters if `cutset` is omitted; otherwise removes trailing characters contained in `cutset`. | *None* | `strings.TrimRightFunc` / `strings.TrimRight` |
| **a.split(sep)** | Splits the string into a list of strings by the delimiter `sep`. | *None* | `strings.Split(a, sep)` |
| **a.split_n(sep, n)** | Splits the string into a list of at most `n` strings by the delimiter `sep`. | *None* | `strings.SplitN(a, sep, n)` |
| **a.join(list)** | Joins a list of strings into a single string using the current string as the separator. | *None* | `strings.Join(list, a)` |
| **a.replace(old, new[, n])** | Replaces occurrences of `old` with `new`. If `n` is provided, replaces at most `n` occurrences; otherwise replaces all. | *None* | `strings.Replace(a, old, new, n)` / `strings.ReplaceAll(a, old, new)` |
| **a.repeat(n)** | Returns a new string consisting of `n` copies of the original string. | *None* | `strings.Repeat(a, n)` |
| **a.substr(start, end)** | Returns the substring of **characters** from `start` (inclusive) to `end` (exclusive). | *None* | *Requires rune conversion/slicing* |
| **a.regex(pattern)** | Returns `true` if the string matches the regex `pattern`. Default behavior is full match; substring match allowed. | `regexec()` | `regexp.MatchString(pattern, a)` |
| **a.regex_split(pattern[, n])** | Splits the string by regex `pattern` into a list of strings. If `n` is provided, splits into at most `n` parts; otherwise splits all occurrences. | `regexec()` + manual split | `regexp.Split(a, n)` |
| **a.regex_groups(pattern)** | Returns a list of capture groups from the first match of `pattern`. Returns empty list if no match. | `regexec()` + `regmatch_t` | `regexp.FindStringSubmatch(a)` |
| **a.regex_replace(pattern, repl[, count])** | Replaces matches of `pattern` with `repl`. If `count` is provided, replaces at most `count` occurrences; otherwise replaces all. | `regsub()` / `regexec()` | `regexp.ReplaceAllString(a, repl)` (custom loop for `count`) |
| **a.tol([base])** | Parses string as a signed 64-bit integer. Optional `base` (0, 2-36). Default is 10 (or auto-detect if 0). | `strtoll(a, ...)` | `strconv.ParseInt(a, base, 64)` |
| **a.tod()** | Parses string as a 64-bit floating point number. | `strtod(a, ...)` | `strconv.ParseFloat(a, 64)` |
| `string sprintf(string fmt, ...)` | Format string with arguments. |
| **int sscanf(string str, string fmt, ...)** | Parse formatted input from string. |
| **string vsprintf(string fmt, va_list args)** | Format string with va_list. |
| **int vsscanf(string str, string fmt, va_list args)** | Parse formatted input from string with va_list. |
