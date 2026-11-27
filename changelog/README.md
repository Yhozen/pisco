# The changelog

Each file here is one entry of the changelog on GitHub Pages (https://yhozen.github.io/pisco/), added in the pull
request it describes. When it reaches `main`, `.github/workflows/changelog.yml` draws the page again
(`tools/changelog.mjs`); nobody edits the `gh-pages` branch by hand.

```markdown
---
title: cowsay, on perl
video: linux-fetch-video-2026-11-27
---
cowsay 3.8.4 on perl 5.44, built from source as a static Linux program: ...

- **lists**, `code` and [links](https://example.org) work; nothing else of Markdown does
```

- **Optional keys:** `prs:` (by default, the pull request that brought the file to `main`: `PR #37`) and `video:` (a
  release tag, below; or a file already on the `gh-pages` branch).
- **The name** is `<yyyy-mm-dd>-<n>-<slug>.md`: newest date first, and the higher `<n>` first within a day.
- **A video goes in a release, never in the repository.** Make a draft release with the video and a `poster.png`
  (`gh release create <tag> --draft --title "..." video.mp4 poster.png`) and name its tag in `video:`; the workflow
  publishes it, with the entry as its notes, when the entry is merged.
- **Check an entry:** `tests/run.sh changelog`, or `node tools/changelog.mjs` to list them all.
