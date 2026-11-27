// SPDX-License-Identifier: GPL-2.0-or-later
// node tools/changelog.mjs [--page <index.html>] [--publish]: the changelog on GitHub Pages, from changelog/*.md.
//   An entry is changelog/<date>[-<n>]-<slug>.md, added in the pull request it describes: front matter (title, and
//   optionally prs: "PRs #28–#34", video: a release tag or a file beside the page), then Markdown (paragraphs, "- "
//   lists, **bold**, `code`, [links](url)). Newest date first; <n> orders a day's entries (higher first). Without
//   prs, the pull request is read from the commit that added the file ("(#37)" or "Merge pull request #37").
//   Without options it checks every entry and prints the list. --page rewrites what is between
//   <!-- changelog:start --> and <!-- changelog:end --> in that file. --publish (with gh and GH_TOKEN) first publishes
//   each video release that is still a draft, with the entry as its notes. .github/workflows/changelog.yml runs both.
import fs from 'node:fs'; import path from 'node:path'; import { execFileSync } from 'node:child_process';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..'), dir = path.join(root, 'changelog');
const repo = process.env.GITHUB_REPOSITORY || 'Yhozen/pisco';
const opt = (k) => { const i = process.argv.indexOf(k); return i < 0 ? null : process.argv[i + 1] ?? true; };
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const inline = (s) => esc(s).replace(/`([^`]+)`/g, '<code>$1</code>').replace(/\*\*(.+?)\*\*/g, '<b>$1</b>')
  .replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, '<a href="$2">$1</a>');
const markdown = (md) => md.trim().split(/\n\s*\n/).map((b) => {
  if (!b.startsWith('- ')) return `<p>${inline(b)}</p>`;
  const items = b.split(/\n(?=- )/).map((i) => `<li>${inline(i.slice(2).replace(/\n\s*/g, ' '))}</li>`);
  return `<ul>\n${items.join('\n')}\n</ul>`;
}).join('\n');
const git = (...a) => { try { return execFileSync('git', a, { cwd: root, encoding: 'utf8' }); } catch { return ''; } };
const prOf = (file) => {
  const s = git('log', '--first-parent', '--diff-filter=A', '--format=%s', '--', path.relative(root, file)).trim().split('\n').pop();
  const m = s.match(/\(#(\d+)\)$/) || s.match(/^Merge pull request #(\d+)/);
  return m ? `PR #${m[1]}` : '';
};

const errors = [];
const entries = fs.readdirSync(dir).filter((f) => f.endsWith('.md') && f !== 'README.md').map((f) => {
  const name = f.match(/^(\d{4}-\d{2}-\d{2})(?:-(\d+))?-[a-z0-9-]+\.md$/), text = fs.readFileSync(path.join(dir, f), 'utf8');
  const fm = text.match(/^---\n([\s\S]*?)\n---\n([\s\S]*)$/);
  if (!name) errors.push(`${f}: the name must be <yyyy-mm-dd>[-<n>]-<slug>.md`);
  if (!fm) { errors.push(`${f}: no front matter (--- title: ... ---)`); return null; }
  const meta = Object.fromEntries(fm[1].split('\n').filter((l) => l.trim()).map((l) => {
    const m = l.match(/^(\w+):\s*(.*?)\s*$/); if (!m) errors.push(`${f}: front matter line "${l}"`);
    return m ? [m[1], m[2].replace(/^"(.*)"$/, '$1')] : [];
  }));
  for (const k of Object.keys(meta)) if (!['title', 'prs', 'video'].includes(k)) errors.push(`${f}: unknown key ${k}`);
  if (!meta.title) errors.push(`${f}: no title`);
  if (!fm[2].trim()) errors.push(`${f}: no text`);
  return { f, date: name?.[1] ?? '', n: +(name?.[2] ?? 0), ...meta, prs: meta.prs ?? prOf(path.join(dir, f)), body: fm[2] };
}).filter(Boolean).sort((a, b) => b.date.localeCompare(a.date) || b.n - a.n);
if (errors.length) { console.error(errors.join('\n')); process.exit(1); }

// a video is a file beside the page (doom-series-web.mp4) or a release tag: its .mp4, and poster.png if it has one
const gh = (...a) => execFileSync('gh', a, { encoding: 'utf8' });
const video = (e) => {
  if (!e.video) return '';
  if (/\.(mp4|webm)$/.test(e.video)) return `<video src="${e.video}" controls playsinline preload="metadata"></video>\n`;
  const r = JSON.parse(gh('release', 'view', e.video, '-R', repo, '--json', 'assets,isDraft'));
  if (r.isDraft && opt('--publish')) {
    const notes = path.join(fs.mkdtempSync(path.join(process.env.RUNNER_TEMP || '/tmp', 'changelog-')), 'notes.md');
    fs.writeFileSync(notes, e.body.trim() + '\n');
    gh('release', 'edit', e.video, '-R', repo, '--draft=false', '--title', `${e.title} (video)`, '--notes-file', notes);
    console.log(`published the release ${e.video}`);
  } else if (r.isDraft) console.log(`${e.video} is still a draft (--publish publishes it)`);
  const url = (n) => `https://github.com/${repo}/releases/download/${e.video}/${n}`;
  const mp4 = r.assets.find((a) => /\.(mp4|webm)$/.test(a.name)), poster = r.assets.find((a) => a.name === 'poster.png');
  if (!mp4) throw new Error(`${e.f}: the release ${e.video} has no video file`);
  return `<video src="${url(mp4.name)}"${poster ? ` poster="${url(poster.name)}"` : ''} controls playsinline preload="metadata"></video>\n`;
};

const page = opt('--page');
if (!page) { for (const e of entries) console.log(`${e.date}  ${e.title}${e.prs ? `  (${e.prs})` : ''}${e.video ? `  video: ${e.video}` : ''}`);
  console.log(`${entries.length} entries in changelog/, each well formed (the page: --page)`); process.exit(0);
}
const html = entries.map((e) => `<h3>${inline(e.title)} <span class="d">${e.date}</span>${e.prs ? `<span class="tag">${esc(e.prs)}</span>` : ''}</h3>\n${video(e)}${markdown(e.body)}\n`).join('\n');
const old = fs.readFileSync(page, 'utf8'), m = old.match(/(<!-- changelog:start -->\n)[\s\S]*?(<!-- changelog:end -->)/);
if (!m) { console.error(`${page}: no <!-- changelog:start --> ... <!-- changelog:end -->`); process.exit(1); }
fs.writeFileSync(page, old.replace(m[0], () => `${m[1]}\n${html}\n${m[2]}`));
console.log(`${page}: ${entries.length} entries`);
