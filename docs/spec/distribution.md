# Distribution

How apps get to someone who does not have this repository: what may be published, and how an app is named.

## What may be published

A recipe names a product and the sha256 of the files it is made from, and holds none of their bytes. **The user
brings their own copy**: the installer they bought, the folder they installed. A recipe, its lock and its sessions
may be published wherever recipes are kept. What is built from the user's files may not, because it is those
files: the `app` layer, the `shaders` layer, and the modules (the app's code, translated). An image marks them
`org.pisco.local-only` ([image.md](image.md#files)), and a builder or a store must not send them anywhere.

This is how the AUR, Lutris and Flathub's `extra-data` work with programs that cannot be redistributed: what is
shared is how to install them, not them.

## Sources

A **source** is a git repository with a `recipes/` folder, each recipe in `recipes/<name>/` (its `name`'s folder),
with its lock and sessions beside it. This repository is one.

- **The official source** is this repository's `recipes/`. Its recipes are reviewed in the pull request that adds
  them, and are checked by its tests (`tests/run.sh spec`, the lock, the levels in [status.md](../status.md#levels)).
- **Any other git repository is a third party's.** No one registers it anywhere: a name leads to it directly, as
  skills.sh names a skill by `owner/repo`, or through a namespace the user gives it, as shadcn's registries do.

A recipe is read at one commit of its source. A builder must record which commit (below) and should let the user
choose it (`pisco install ... --rev <commit | branch | tag>`). Bases never come from a source: a recipe names one
(`from`) and the builder's own base is used, so a source can change what an app is made from, but not what runs
it.

### Third-party recipes

A recipe of a source other than the official one must be refused when:

- an input is `built_by`: that runs a script of the source's on the user's machine. A third party's inputs are the
  user's files, known by their sha256;
- an input's `file`, or `warmup.session`, leads outside the recipe's folder (`/`, `\`, `..`): an input is looked
  for by its plain file name;
- it is not in the folder of its `name`.

A builder must say that a recipe is a third party's before it uses the user's files. Everything else a recipe may
say holds as for any recipe: it is read strictly, and it runs no code ([README.md](README.md)).

## Names

An app is installed by a **name** that leads to a recipe:

| Name | The recipe |
|---|---|
| `<app>` | `recipes/<app>/` of the official source |
| `<owner>/<repo>/<app>` | `recipes/<app>/` of `https://github.com/<owner>/<repo>` |
| `@<ns>/<app>` | `recipes/<app>/` of the git repository the user named `@<ns>` (`pisco source add @<ns> <git url>`) |
| `<git url>#<app>` | `recipes/<app>/` of any git repository |
| a folder | the recipe in that folder (`recipe.toml`) |

`<app>` and `<ns>` are lower-case letters, digits and `-`, as a recipe's `name` is ([recipe.md](recipe.md)). Once
installed, the app is known by its `name` alone, whatever name led to it; a second install of that `name` replaces
the first, and keeps what the app saved.

An installed image's entry in `index.json` says where its recipe came from, as `org.pisco.source`: `official`,
`dir:<the folder>`, or `git:<url>#<app>@<commit>`.
