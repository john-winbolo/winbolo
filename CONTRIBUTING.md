# Contributing to WinBolo

Bug reports, fixes, new features, maps, translations and documentation changes
are all welcome. This guide explains how to send a change and what a pull
request needs before it can be merged.

## Before you start

- **Search first.** Check the existing issues and pull requests to see whether
  someone has already reported the problem or started the work.
- **Talk about large changes before writing them.** For a new feature, a
  change to how the game plays, or anything that touches the areas listed
  under [Compatibility](#compatibility), open an issue or ask on the
  [WinBolo Discord](https://discord.gg/znGR3VMaqd) first. Agree what the change
  should do before you spend time on it. A large pull request that nobody asked
  for may wait a long time for review, or be closed.
- **Small fixes need no discussion.** A bug fix, a typo, or a translation
  correction can go straight to a pull request.

Read [docs/BUILDING.md](docs/BUILDING.md) to set up a build, and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) before changing anything under
`src/`. The architecture document says which directories may include which
headers, and the build enforces most of it.

## How to send a change

1. Fork the repository on GitHub.
2. Create a branch in your fork for the change. Use one branch per change.
3. Make the change, build it, and run the tests (see [Testing](#testing)).
4. Push the branch to your fork and open a pull request against `main`.
5. Reply to review comments by pushing more commits to the same branch. There
   is no need to open a new pull request.

Pull requests are squash-merged, so your branch becomes a single commit on
`main`. Keep each pull request to one change. If you find an unrelated bug
along the way, send it as a separate pull request.

Keep tidy-up work separate from changes in behaviour. A pull request that
reformats or renames code should not also change what the game does, because
the behaviour change is then hard to find in review.

### Commit messages and pull request titles

Write the title as what you did and the problem it solves, in the past tense,
for example:

- `Fixed pillboxes shooting through walls on the map edge`
- `Added a mute button to the players panel`

The description should say why the change was needed, how you tested it, and
anything you did not test.

## What a pull request should include

- A short summary of the change and the reason for it.
- How you tested it: the commands you ran, the platform you ran them on, and
  whether you played a game with the change. Say which checks you did not run.
  A successful build is not a test.
- Documentation changes, if the change affects anything the documentation
  describes: building, command-line options, the scenario API, skins, file
  formats or settings.
- Screenshots or a short recording for a change you can see in the game.

## Code style

- The game and server are written in C99. The ImGui interface is C++17.
- Match the code around your change: its naming, layout, comment style and
  the way it handles errors. Do not reformat code you are not otherwise
  changing.
- The build uses `-Wall -Wextra`. Do not add new warnings.
- Reach the simulation through the public headers in `src/bolo/public/`. If
  you need something that is only in `src/bolo/internal/`, add a function to
  the public API instead of including the internal header. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- Every user-visible string goes through the translation table. Add a `STR_*`
  id to `src/gui/lang.h` and its English text to `src/gui/sdl3/lang.c`, then
  run `python3 tools/dump_lang_en.py` to regenerate `data/lang/en.txt`. See
  [docs/TOOLS.md](docs/TOOLS.md#toolsdump_lang_enpy).

## Compatibility

Some changes can break things without any build error. Take extra care with:

- **The network protocol.** Players on different builds play together. A
  change to a packet has to work with clients and servers that do not have it.
- **The simulation.** The same inputs and seed must give the same game on every
  platform. The baseline tests compare games against recorded output byte for
  byte, so a change here shows up as a test failure; if the change is
  intended, re-record the output and explain why in the pull request.
- **File formats.** Maps, replays (`.wbv`), skins and scenario packages are
  shared between players and kept for years. Old files must keep loading.
- **The scenario API and the bot brain API.** Scripts written by other people
  depend on them. See [docs/SCENARIO_API.md](docs/SCENARIO_API.md).
- **Saved settings.** A player's existing preferences must still load after an
  upgrade.

If your change affects one of these, say so in the pull request and describe
what happens to older clients, servers or files.

## Testing

Build the unit tests and the two programs they depend on, then run the suite:

```bash
cmake --build build --target WinBoloUnitTests WinBoloHeadless WinBoloDS
ctest --test-dir build -j8
```

Add a test for a bug fix where you can, so the bug does not come back. Unit
tests live in `tests/unit/`. Tests that play a whole game with bots are the
baseline scenarios in `tests/baseline/`; the steps for adding one are in
[docs/BUILDING.md](docs/BUILDING.md#adding-a-new-baseline-scenario). See
[tests/README.md](tests/README.md) for the headless client they use.

For a change you can see or hear in the game, also play a game with it before
sending the pull request.

## What not to commit

- Build directories or any other build output
- The Steamworks SDK, or any other SDK that is not open source
- Passwords, API keys, Sentry DSNs, signing certificates or other credentials
- Personal data, including player logs and replays from real games
- IDE settings

Name each file when you stage it (`git add path/to/file`) rather than using
`git add -A` or `git add .`, which pick up build directories.

## Using AI tools

You are welcome to use AI tools such as Claude, ChatGPT or Copilot to help
write code, tests and documentation.

You are responsible for everything you submit, whether or not an AI tool
helped write it. Before you open a pull request you must:

- understand every line of the change and be able to explain why it is
  written the way it is;
- have checked that it works, by building it, running the tests and playing
  the game where that applies. An AI tool saying the code works is not a test;
- be ready to answer review questions about it yourself, and to fix bugs in the
  feature after it is merged.

If you could not maintain the change without the AI tool, do not submit it.

Write pull request descriptions and review replies yourself, and keep them
short. A few sentences in your own words are more useful than a long generated
summary. Do not add AI attribution lines such as `Co-Authored-By` or
"Generated with" to commits or pull requests. The change is yours, not the AI
tool's: "the AI wrote that part" is not an answer to a review question or a bug
report.

## Licence

WinBolo is licensed under the GNU General Public License, version 3 or (at your
option) any later version. See [LICENSE](LICENSE).

By submitting a contribution, you confirm that:

- you wrote it yourself, or otherwise have the right to submit it;
- it does not include code, artwork or sound copied from anywhere that does
  not allow it to be distributed under the GPL; and
- you agree that it will be distributed as part of WinBolo under the GNU
  General Public License, version 3 or any later version, together with the
  additional permissions in [LICENSE-EXCEPTION.md](LICENSE-EXCEPTION.md) for
  platform SDKs such as the Steamworks SDK, app stores and locked devices.

If your contribution includes code or assets from another project, name the
source and its licence in the pull request, and add it to
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
