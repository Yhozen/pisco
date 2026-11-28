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

## Names

An app is installed by a **name** that leads to a recipe:

| Name | The recipe |
|---|---|
| `<app>` | `recipes/<app>/` of the official source: the recipes of this repository, reviewed in its pull requests |
| a folder | the recipe in that folder (`recipe.toml`) |

`<app>` is the recipe's `name` ([recipe.md](recipe.md)): lower-case letters, digits and `-`. Once installed, the app
is known by its `name` alone, whatever name led to it; a second install of that `name` replaces the first, and
keeps what the app saved.

An installed image's entry in `index.json` says where its recipe came from, as `org.pisco.source`: `official`, or
`dir:<the folder>`.
