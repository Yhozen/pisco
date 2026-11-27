---
title: Tag in the browser, faster
prs: PRs #26, #27
---
- Tag: The Power of Paint was played by hand through level 3. A Chrome profile showed its thread waiting on 21,000
  file checks to the server and its whole 384 KiB vertex buffer re-sent a dozen times a frame; file checks are now
  answered once and only the changed bytes are sent (50 GB to 1.1 GB over a session).
- Windows x64 titles get a window again; the docs say where every runtime stands, and what to do if the browser
  ever becomes the limit.
