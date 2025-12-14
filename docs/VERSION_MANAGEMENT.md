# Version Management for Source Tarball

**Problem:** When Clonezilla team downloads source tarball from GitHub tag releases, there's no `.git` directory, so `git describe` fails and version falls back to hardcoded value.

**Current Implementation (CMakeLists.txt:49-58):**
```cmake
execute_process(COMMAND git describe --tags --dirty
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(GIT_VERSION STREQUAL "")
    set(GIT_VERSION 2.0.16)  # ❌ Hardcoded fallback
endif()
```

---

## Solution 1: VERSION File (Simple, Manual)

**Pros:** Simple, widely used
**Cons:** Must manually update before each release

### Implementation

#### Step 1: Create VERSION file

```bash
# In repo root
echo "2.0.16" > VERSION
git add VERSION
git commit -m "Add VERSION file for tarball releases"
```

#### Step 2: Update CMakeLists.txt

```cmake
# Read VERSION file first
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VERSION_FROM_FILE)
    string(STRIP "${VERSION_FROM_FILE}" VERSION_FROM_FILE)
endif()

# Try git describe
execute_process(COMMAND git describe --tags --dirty
    TIMEOUT 5
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Priority: git > VERSION file > hardcoded
if(NOT GIT_VERSION STREQUAL "")
    # Git repo - use git describe
    set(EZIO_VERSION "${GIT_VERSION}")
elseif(VERSION_FROM_FILE)
    # Source tarball - use VERSION file
    set(EZIO_VERSION "${VERSION_FROM_FILE}")
else()
    # Fallback
    set(EZIO_VERSION "unknown")
endif()

target_compile_definitions(${EZIO} PUBLIC
    GIT_VERSION="${EZIO_VERSION}"
)
```

#### Step 3: Update VERSION before each release

```bash
# Before tagging
echo "2.0.17" > VERSION
git add VERSION
git commit -m "Bump version to 2.0.17"
git tag v2.0.17
git push origin v2.0.17
```

---

## Solution 2: git archive + export-subst (Recommended) ✅

**Pros:** Fully automatic, GitHub releases natively supported, no manual updates
**Cons:** Slightly more complex setup

### How It Works

1. Git has a feature called `export-subst` in `.gitattributes`
2. When creating tarball via `git archive`, Git replaces placeholders with real values
3. GitHub releases use `git archive` internally → automatic!

### Implementation

#### Step 1: Create version.txt file

```bash
# Create template with git placeholders
# %D = ref names (branch, tag) - example output: "HEAD -> master, tag: v2.0.16"
cat > version.txt << 'EOF'
$Format:%D$
EOF

git add version.txt
```

**What is `%D`?**
- `%D` is a git format placeholder (same as `git log --pretty=format:%D`)
- Means: ref names (branch and tag names)
- After `git archive`, it becomes: `HEAD -> master, tag: v2.0.16, origin/master`
- CMake extracts the tag version from this string

#### Step 2: Create/update .gitattributes

```bash
# Create or append to .gitattributes
echo 'version.txt export-subst' >> .gitattributes
git add .gitattributes
```

#### Step 3: Update CMakeLists.txt

```cmake
# Function to extract version from .version_template (tarball)
function(get_version_from_template OUTPUT_VAR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.version_template")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.version_template" VERSION_TEMPLATE)

        # Template format: "HEAD -> master, tag: v2.0.16, origin/master"
        # or: "$Format:%D$" (not yet substituted)

        if(VERSION_TEMPLATE MATCHES "\\$Format:")
            # Not substituted - not from tarball
            set(${OUTPUT_VAR} "" PARENT_SCOPE)
        elseif(VERSION_TEMPLATE MATCHES "tag: v?([0-9]+\\.[0-9]+\\.[0-9]+[^,)]*)")
            # Extracted version from tag
            set(${OUTPUT_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        else()
            set(${OUTPUT_VAR} "" PARENT_SCOPE)
        endif()
    else()
        set(${OUTPUT_VAR} "" PARENT_SCOPE)
    endif()
endfunction()

# Try git describe first
execute_process(COMMAND git describe --tags --dirty
    TIMEOUT 5
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Try extracting from .version_template (tarball)
if(GIT_VERSION STREQUAL "")
    get_version_from_template(TARBALL_VERSION)
endif()

# Priority: git > tarball template > fallback
if(NOT GIT_VERSION STREQUAL "")
    set(EZIO_VERSION "${GIT_VERSION}")
elseif(TARBALL_VERSION)
    set(EZIO_VERSION "v${TARBALL_VERSION}")
else()
    set(EZIO_VERSION "2.0.16-unknown")
endif()

message(STATUS "EZIO version: ${EZIO_VERSION}")

target_compile_definitions(${EZIO} PUBLIC
    GIT_VERSION="${EZIO_VERSION}"
)
```

#### Step 4: Test locally

```bash
# Simulate GitHub release tarball
git archive --format=tar.gz --prefix=ezio-2.0.16/ v2.0.16 > ezio-2.0.16.tar.gz

# Extract and test
tar xzf ezio-2.0.16.tar.gz
cd ezio-2.0.16
cat .version_template  # Should show: HEAD -> master, tag: v2.0.16, ...

# Build and verify
mkdir build && cd build
cmake ..
# Should see: EZIO version: v2.0.16
```

#### Step 5: GitHub Releases (automatic!)

When you create a release on GitHub:
1. GitHub automatically generates tarball using `git archive`
2. `.version_template` is automatically filled with tag info
3. CMake extracts version from template
4. ✅ No manual intervention needed!

---

## Solution 3: VERSION File + Git Hook (Semi-automatic)

**Pros:** Automatic version updates, easy to understand
**Cons:** Requires git hooks (may be forgotten)

### Implementation

#### Step 1: Create VERSION file

```bash
echo "2.0.16" > VERSION
git add VERSION
```

#### Step 2: Create pre-tag hook

```bash
# .git/hooks/pre-commit or use pre-tag hook
cat > .git/hooks/update-version << 'EOF'
#!/bin/bash
# Extract version from latest tag
VERSION=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
if [ -n "$VERSION" ]; then
    echo "$VERSION" > VERSION
    git add VERSION
fi
EOF

chmod +x .git/hooks/update-version
```

#### Step 3: Manual workflow

```bash
# Tag first
git tag v2.0.17

# Update VERSION
git describe --tags --abbrev=0 | sed 's/^v//' > VERSION
git add VERSION
git commit -m "Update VERSION to 2.0.17"
git tag -f v2.0.17  # Move tag
git push origin v2.0.17
```

---

## Solution 4: GitHub Actions Generate Custom Tarball

**Pros:** Full control, can inject any metadata
**Cons:** More complex, requires GitHub Actions

### Implementation

Create `.github/workflows/release.yml`:

```yaml
name: Release

on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
        with:
          fetch-depth: 0  # Full history for git describe

      - name: Generate VERSION file
        run: |
          VERSION=$(git describe --tags)
          echo "$VERSION" > VERSION
          cat VERSION

      - name: Create source tarball
        run: |
          TAG_NAME=${GITHUB_REF#refs/tags/}
          tar czf ezio-${TAG_NAME}.tar.gz \
            --exclude='.git' \
            --exclude='.github' \
            --transform "s,^,ezio-${TAG_NAME}/," \
            .

      - name: Upload to release
        uses: softprops/action-gh-release@v1
        with:
          files: ezio-*.tar.gz
```

---

## Comparison

| Solution | Automatic | GitHub Native | Maintenance | Complexity |
|----------|-----------|---------------|-------------|------------|
| 1. VERSION file | ❌ Manual | ✅ Yes | Manual update before tag | ⭐ Simple |
| 2. export-subst | ✅ Auto | ✅ Yes | Zero | ⭐⭐ Medium |
| 3. VERSION + hook | ⚠️ Semi | ✅ Yes | Hook setup | ⭐⭐ Medium |
| 4. GitHub Actions | ✅ Auto | ⚠️ Custom | GitHub Actions | ⭐⭐⭐ Complex |

---

## Recommendation

**Use Solution 2 (export-subst)** ✅

**Why:**
1. **Zero maintenance**: No manual VERSION file updates
2. **GitHub native**: Works with standard GitHub release tarballs
3. **Transparent**: Developers don't need to think about it
4. **Battle-tested**: Used by many projects (Linux kernel, Git itself)

**Migration steps:**
1. Create `.version_template` with `$Format:%D$`
2. Add to `.gitattributes`: `.version_template export-subst`
3. Update CMakeLists.txt with version extraction logic
4. Test with `git archive`
5. Push changes
6. Next release: GitHub tarball will have version automatically!

---

## Testing

### Test git repo (developer)
```bash
cd /path/to/ezio
mkdir build && cd build
cmake ..
# Should show: EZIO version: v2.0.16-13-g41c4baf
```

### Test tarball (Clonezilla team)
```bash
# Download from GitHub releases
wget https://github.com/ntu-as-cooklab/ezio/archive/refs/tags/v2.0.16.tar.gz
tar xzf v2.0.16.tar.gz
cd ezio-2.0.16
mkdir build && cd build
cmake ..
# Should show: EZIO version: v2.0.16
```

---

## References

- [git-archive documentation](https://git-scm.com/docs/git-archive)
- [gitattributes export-subst](https://git-scm.com/docs/gitattributes#_creating_an_archive)
- [CMake version handling best practices](https://cmake.org/cmake/help/latest/command/project.html#version-handling)

---

**Document Version:** 1.0
**Last Updated:** 2024-12-14
**Status:** Implementation Guide
