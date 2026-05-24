# Linguist submission steps

This directory contains everything needed to submit toke to GitHub Linguist.
The PR has NOT been submitted -- follow these steps when ready.

## Pre-flight checks

1. **Adoption threshold** -- Linguist requires at least 200 unique `.tk` files
   indexed on GitHub (excluding forks) in the last year. Check:
   https://github.com/search?q=extension%3Atk+NOT+fork%3Atrue&type=code

2. **Tree-sitter grammar license** -- must be one of Linguist's approved
   licenses. The grammar at `tree-sitter-toke/` is ISC, which is approved.

3. **No extension conflicts** -- `.tk` is not currently claimed by another
   language in Linguist.

## Submission steps

```bash
# 1. Fork github-linguist/linguist and clone it
gh repo fork github-linguist/linguist --clone
cd linguist

# 2. Create a branch
git checkout -b add-toke-language

# 3. Add the languages.yml entry (without language_id)
#    Insert alphabetically after "Turing", before "Turtle"
#    Use the entry from languages.yml in this directory but
#    OMIT the language_id line -- the script generates it.

# 4. Add the tree-sitter grammar
script/add-grammar https://github.com/karwalski/toke.git#tree-sitter-toke

# 5. Copy sample files
cp /path/to/linguist-submission/samples/toke/* samples/toke/

# 6. Generate the language_id
script/update-ids

# 7. Run tests
bundle exec rake test

# 8. Commit and push
git add -A
git commit -m "Add toke programming language"
git push origin add-toke-language

# 9. Open PR using the description in PR-DESCRIPTION.md
gh pr create --title "Add toke programming language" \
  --body-file /path/to/linguist-submission/PR-DESCRIPTION.md
```

## File inventory

| File | Purpose |
|------|---------|
| `languages.yml` | Entry to merge into Linguist's `lib/linguist/languages.yml` |
| `PR-DESCRIPTION.md` | PR body text for the GitHub pull request |
| `samples/toke/*.tk` | 5 representative sample files for the `samples/` directory |
| `SUBMISSION-STEPS.md` | This file |

## Grammar reference

The tree-sitter grammar lives at `/Users/matthew.watt/tk/toke/tree-sitter-toke/`
and is already configured with:
- Scope: `source.toke`
- File type: `.tk`
- Highlights query: `queries/highlights.scm`
- License: ISC
