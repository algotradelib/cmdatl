# Guides and skills

Two guides teach cmdatl to an AI agent: the agent guide (reading the account, the order rails, exit codes, runs) and the strategy guide (atls, run specs, backtests, runs).

```sh
cmdatl guide agent # the guide of this binary
cmdatl guide strategy --format text # bare text, also through a pipe
```

- They are the same text as the Agent Skills in `skills/` (`cmdatl-agent` and `cmdatl-strategy`), built into the binary from those files.
- The repository is a Claude Code plugin: `claude --plugin-dir /path/to/cmdatl` loads both skills from a clone.
- For tools that read a project's `.agents/skills/`, copy the two folders from `skills/`, or from `share/cmdatl/skills` where cmdatl is installed.
