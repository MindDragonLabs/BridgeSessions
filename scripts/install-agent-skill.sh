#!/usr/bin/env bash
# Install BridgeSessions agent skill into common harness search paths.
# Run from repo root: ./scripts/install-agent-skill.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKILL_SRC="$ROOT/skills/bridgesessions"
test -f "$SKILL_SRC/SKILL.md" || { echo "missing $SKILL_SRC/SKILL.md"; exit 1; }

link() {
  local dest="$1"
  mkdir -p "$(dirname "$dest")"
  ln -sfn "$SKILL_SRC" "$dest"
  echo "linked $dest -> $SKILL_SRC"
}

# In-repo discovery (relative from repo)
cd "$ROOT"
mkdir -p .claude/skills .opencode/skills .agents/skills .codex/skills
ln -sfn ../../skills/bridgesessions .claude/skills/bridgesessions
ln -sfn ../../skills/bridgesessions .opencode/skills/bridgesessions
ln -sfn ../../skills/bridgesessions .agents/skills/bridgesessions
ln -sfn ../../skills/bridgesessions .codex/skills/bridgesessions
ln -sfn AGENTS.md CLAUDE.md 2>/dev/null || true
echo "in-repo harness links OK"

# Hermes (user profile)
if [[ -d "${HOME}/.hermes/skills" ]]; then
  mkdir -p "${HOME}/.hermes/skills/devops"
  link "${HOME}/.hermes/skills/devops/bridgesessions"
fi

# OpenCode global
if [[ -d "${HOME}/.config/opencode" ]] || mkdir -p "${HOME}/.config/opencode/skills" 2>/dev/null; then
  mkdir -p "${HOME}/.config/opencode/skills"
  link "${HOME}/.config/opencode/skills/bridgesessions"
fi

# Claude global skills (if used)
if [[ -d "${HOME}/.claude/skills" ]] || mkdir -p "${HOME}/.claude/skills" 2>/dev/null; then
  link "${HOME}/.claude/skills/bridgesessions"
fi

echo "Done. Skill name: bridgesessions"
echo "  AGENTS.md always-on: $ROOT/AGENTS.md"
echo "  SKILL.md: $SKILL_SRC/SKILL.md"
