# CLAUDE.md

## Commit messages

Every commit message must end with these fields, in this order, in
`key=value` form. The same format is in the commit template `.gitmessage`
(enable it once per clone with `git config commit.template "$PWD/.gitmessage"`).

```
PR=PR#<number>
Test=<what was actually built/run>
Tested-by=<tester name> <tester email>
Reviewed-by=<reviewer name> <reviewer email>
```

- **PR=**: written as `PR=PR#<number>`, e.g. `PR=PR#41`. Leave it empty until
  the PR exists. Once the number is known, update the branch's commits.
- **Test=**: only builds and tests that were actually run (e.g. x86_64 build
  against the 6.18 headers, arm64 RPi cross-compile, `insmod`/`rmmod` on the
  RPi). Say "Not run on hardware." until a hardware test has been done.
- **Tested-by=**: name and email of the person who actually did the testing,
  not automatically the committer. Leave it empty if unknown.
- **Reviewed-by=**: name and email of the reviewer.
- Keep these fields up to date without being asked: add them to every new
  commit, and when new information arrives (PR created, test done, review
  given), reword the affected commits. Rewording pushed commits needs a
  force-push.
- Do not add a `Co-Authored-By` line.
