# Using Claude to maintain Alice68

The repository is set up so that Claude can maintain it from one instruction:
[`CLAUDE.md`](../CLAUDE.md) tells it how to build and test Alice68, the rules that keep it
working and properly licensed, and step by step how to bring every upstream project up to
date. This page shows how to point Claude at the repository, in three ways. Pick one.

| Way | Good for | You need |
|---|---|---|
| **A. Claude Code on the web** | the easiest: everything in the browser | a Claude Pro, Max, Ultra or Team plan |
| **B. Claude Code on your computer** | working alongside Claude, local builds | the same, or a Claude API key |
| **C. `@claude` in a GitHub issue** | asking from GitHub, also from a phone | admin rights on the repository (your fork, if you're not the owner), a plan or an API key |

If you are not the owner of `farsonic/Alice68`, **fork it first** (the **Fork** button on
GitHub) and use your fork: Claude needs to be able to push to the repository it works on.
Send improvements back as a pull request.

## What to say

Once Claude has the repository, these are the instructions it knows:

| Say | Claude does |
|---|---|
| `Bring everything up to date` | checks every upstream project (Emu68, AGA-PiStorm, FRF's EMU68AGA Bare Metal, Neptune, the Emu68 driver stack, TinyUSB, the Arm toolchain), takes in what's new, builds and tests everything, updates the notes, pushes, and reports. It asks before changing `main` or making a release. |
| `Bring everything up to date and release it` | the same, then packages a release zip. |
| `Check upstream, change nothing` | only reports what's new. |

**AGA-PiStorm** is published on [itch.io](https://astair86.itch.io/aga-pistorm-10-experimental-prototype),
not on GitHub. If there's a new version, Claude may ask you for it: download the
`AGA-Pistorm_universal_vX.7z` and give Claude the file or the link.

Nothing here can boot a Raspberry Pi. Claude says what it tested and what needs a real
Pi 4. Try those things before you release.

## A. Claude Code on the web

1. Open **<https://claude.ai/code>** and sign in.
2. Connect GitHub when asked. For a **private** repository, install the Claude GitHub App
   for it: <https://github.com/apps/claude/installations/new>. Public repositories work
   without it.
3. In the **repository selector** under the message box, choose `farsonic/Alice68` (or your
   fork). The branch can stay `main`.
4. Choose **Auto** as the permission mode, so Claude can run the builds and tests without
   asking about each command.
5. Type the instruction and press Enter:
   ```
   Bring everything up to date
   ```
6. Claude works in its own cloud machine, installs the toolchain it needs (the steps are in
   `CLAUDE.md`), and pushes its work to a branch. Read its report. When you're happy, press
   **Create PR** on the changes card, then merge the pull request on GitHub.

## B. Claude Code on your computer

Install it (macOS, Linux, WSL):

```bash
curl -fsSL https://claude.ai/install.sh | bash
```

Windows PowerShell:

```powershell
irm https://claude.ai/install.ps1 | iex
```

Other ways (Homebrew, WinGet, apt): <https://code.claude.com/docs/en/setup>.

Then clone the repository and start Claude in it:

```bash
git clone https://github.com/farsonic/Alice68.git
```
```bash
cd Alice68 && claude
```

Sign in when it asks, and type `Bring everything up to date`. On your own computer Claude
needs Ubuntu 24.04 (or WSL with Ubuntu) to build Alice68; it installs the packages and the
Arm toolchain listed in `CLAUDE.md`, asking you before running commands unless you allow
them.

## C. `@claude` in a GitHub issue

This uses the Claude Code GitHub Action: Claude runs on GitHub's own machines whenever
someone with write access mentions `@claude`.

### Set it up once

**Quick way:** install Claude Code (B above) and the GitHub CLI, then in the repository:

```bash
gh auth login
```
```bash
claude
```

and inside Claude type `/install-github-app`. It installs the Claude GitHub App, stores your
key as a repository secret, and opens a pull request adding the workflow. **Merge that pull
request.** You need admin rights on the repository.

**By hand:**

1. Install the Claude GitHub App on the repository: <https://github.com/apps/claude>.
2. In the repository, **Settings → Secrets and variables → Actions → New repository
   secret**, add **one** of:
   - `CLAUDE_CODE_OAUTH_TOKEN`: for a Claude Pro, Max, Ultra, Team or Enterprise plan. Make
     the token with `claude setup-token` in Claude Code.
   - `ANTHROPIC_API_KEY`: an API key from the Claude Console, <https://platform.claude.com>.
3. Add the file `.github/workflows/claude.yml`:

```yaml
name: Claude Code
on:
  issue_comment:
    types: [created]
  pull_request_review_comment:
    types: [created]
  issues:
    types: [opened]
jobs:
  claude:
    if: >-
      (github.event_name == 'issues' && contains(github.event.issue.body, '@claude')) ||
      (github.event_name != 'issues' && contains(github.event.comment.body, '@claude'))
    runs-on: ubuntu-latest
    timeout-minutes: 240
    permissions:
      contents: write
      pull-requests: write
      issues: write
      id-token: write
      actions: read
    steps:
      - uses: actions/checkout@v6
        with:
          fetch-depth: 0
      - uses: anthropics/claude-code-action@v1
        with:
          # with a plan:
          claude_code_oauth_token: ${{ secrets.CLAUDE_CODE_OAUTH_TOKEN }}
          # or with an API key instead:
          # anthropic_api_key: ${{ secrets.ANTHROPIC_API_KEY }}
          claude_args: "--max-turns 400"
```

`fetch-depth: 0` gives Claude the whole history (it needs the `upstream` branch), the long
`timeout-minutes` and `--max-turns` leave room for a full build and test run.

### Use it

Open a new issue (or comment on one) with:

```
@claude Bring everything up to date
```

Claude replies in the issue as it works, pushes a branch and opens a pull request with its
report. Review it, test the kernel on a Pi if the report says so, and merge.

## After Claude is done: releasing

Claude can build the release zip, but tags and releases are made on GitHub:

1. **Releases → Draft a new release**: create the tag (e.g. `v0.4.0`) on the commit Claude
   names (the kernels report it as `git:<commit>`).
2. Attach the zip (`Alice68-x.y.z-Pi4.zip`) and paste the release text Claude gives you.

## More

- How Claude Code works with GitHub: <https://code.claude.com/docs/en/github-actions>,
  <https://code.claude.com/docs/en/claude-code-on-the-web>
- What Claude follows in this repository: [`CLAUDE.md`](../CLAUDE.md)
- The upstream list it checks: [`tools/upstreams.tsv`](../tools/upstreams.tsv)
