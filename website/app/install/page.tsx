import type { Metadata } from "next";
import { Command, Section } from "@/components/Blocks";
import {
  AGENT_SKILL_INSTALL_URL,
  BINARY,
  CLI,
  CUA_URL,
  INSTALL_PS1,
  INSTALL_SH,
  ISSUES_URL,
  LINUX_MAC_BIN,
  LINUX_MAC_CLI,
  PRODUCT,
  RECOVERY_LINUX_STOP_DAEMON,
  RECOVERY_MAC_BOOTOUT_CUA,
  RECOVERY_MAC_BOOTOUT_MENUBAR,
  RECOVERY_MAC_BOOTOUT_MESH,
  RECOVERY_WIN_END_CUA_TASK,
  RECOVERY_WIN_END_MESH_TASK,
  RECOVERY_WIN_KILL_HELPER,
  RECOVERY_WIN_STOP_TRAY,
  RELEASE_TAG_URL,
  SHA256SUMS_URL,
  SHIPPING_ARCH_LINE,
  SKILL_URL,
  STATE_DIR,
  WINDOWS_BIN,
} from "@/lib/product";

export const metadata: Metadata = {
  title: `Install ${PRODUCT}`,
  description: "Install BridgeSessions, then join a machine, run a command, move a file, or look at the screen.",
};

export default function InstallPage() {
  return (
    <div className="mx-auto max-w-3xl px-4 py-12 sm:px-6 sm:py-16">
      <h1 className="font-display text-5xl text-paper">Install</h1>
      <p className="mt-4 max-w-xl text-[15px] leading-relaxed text-steel">
        One installer. Paste it. Then type <span className="font-mono text-paper">{CLI} --version</span>.
        That is the whole first step.
      </p>

      <Section id="get" kicker="01" title="Get the program">
        <p className="mb-3 font-mono text-[11px] tracking-[0.12em] text-mute uppercase">
          Linux and Mac
        </p>
        <Command text={INSTALL_SH} />
        <p className="mt-4 mb-3 font-mono text-[11px] tracking-[0.12em] text-mute uppercase">
          Windows
        </p>
        <Command text={INSTALL_PS1} />
        <p className="mt-4 text-[15px] leading-relaxed text-steel">
          It drops the program at <span className="font-mono text-paper">{LINUX_MAC_BIN}</span>{" "}
          (<span className="font-mono text-paper">{LINUX_MAC_CLI}</span> is the short name) on
          Linux and Mac, or <span className="font-mono text-paper">{WINDOWS_BIN}</span> on
          Windows. If <span className="font-mono text-paper">bs</span> is not found, add that
          folder to your PATH and open a new terminal.
        </p>
        <p className="mt-4 text-[15px] leading-relaxed text-steel">
          Prefer a file you can see first? The{" "}
          <a href={RELEASE_TAG_URL} rel="noopener noreferrer" className="text-paper underline">
            GitHub release
          </a>{" "}
          has the three builds: {SHIPPING_ARCH_LINE}.
        </p>
      </Section>

      <Section id="verify" kicker="02" title="Make sure it is the real file">
        <p className="mb-4 text-[15px] leading-relaxed text-steel">
          Download{" "}
          <a href={SHA256SUMS_URL} rel="noopener noreferrer" className="text-paper underline">
            SHA256SUMS
          </a>{" "}
          next to the binary. Then:
        </p>
        <Command text={`sha256sum -c SHA256SUMS --ignore-missing`} />
        <p className="mt-4 mb-3 font-mono text-[11px] tracking-[0.12em] text-mute uppercase">
          Mac
        </p>
        <Command text={`shasum -a 256 -c SHA256SUMS`} />
        <p className="mt-4 mb-3 font-mono text-[11px] tracking-[0.12em] text-mute uppercase">
          Windows
        </p>
        <Command
          text={`Get-FileHash .\\bridgesessions-windows-x86_64.exe -Algorithm SHA256`}
        />
      </Section>

      <Section id="check" kicker="03" title="See that it runs">
        <Command text={`${CLI} --version`} />
        <Command text={`${CLI} doctor`} />
        <p className="mt-4 text-[15px] leading-relaxed text-steel">
          Doctor tells you if the install looks healthy. If something is off, it
          usually says what to fix.
        </p>
      </Section>

      <Section id="join" kicker="04" title="Join another machine">
        <p className="mb-4 text-[15px] leading-relaxed text-steel">
          On the machine you want to reach, start the program and leave it
          running. Then pair from your laptop. The first time, you pin that
          machine on purpose so you know who you are talking to.
        </p>
        <Command text={`${BINARY} --daemon`} />
        <Command text={`${CLI} join`} />
        <Command text={`${CLI} peers list`} />
        <p className="mt-4 text-[15px] leading-relaxed text-steel">
          After that, use the name you see in the list. Do not guess if two names
          look close.
        </p>
      </Section>

      <Section id="shell" kicker="05" title="Run something over there">
        <p className="mb-4 text-[15px] leading-relaxed text-steel">
          Stack the work in one command. Opening a folder in one call and
          running the job in the next does not keep that folder.
        </p>
        <Command text={`${CLI} shell <peer> --cmd "uname -a"`} />
        <Command
          text={`${CLI} shell <peer> --cmd "bash -lc 'cd /app && npm install && npm test'"`}
        />
        <p className="mt-4 text-[15px] leading-relaxed text-steel">
          Longer than a couple of lines? Send a script instead.
        </p>
        <Command text={`${CLI} run-script <peer> deploy.sh`} />
      </Section>

      <Section id="file" kicker="06" title="Move a file">
        <p className="mb-4 text-[15px] leading-relaxed text-steel">
          Same program. Send something over, or pull a log or a screenshot back.
          Wait for it to finish.
        </p>
        <Command
          text={`${CLI} file send <peer> ./local.bin /remote/path/local.bin --wait`}
        />
        <Command
          text={`${CLI} file recv <peer> /remote/path/app.log ./app.log --wait`}
        />
      </Section>

      <Section id="capture" kicker="07" title="See the screen. Click if you have to.">
        <p className="mb-4 text-[15px] leading-relaxed text-steel">
          On a Mac or Windows desktop, look first. Then click. On those
          machines the helper has to be running in the logged-in user session.
        </p>
        <Command text={`${CLI} cua screen <peer>`} />
        <Command text={`${CLI} cua capture <peer>`} />
        <Command text={`${CLI} cua click <peer> --x 100 --y 200`} />
        <p className="mt-4 text-[15px] leading-relaxed text-steel">
          More on that in the{" "}
          <a href={CUA_URL} rel="noopener noreferrer" className="text-paper underline">
            desktop notes
          </a>
          .
        </p>
      </Section>

      <Section id="agents" kicker="08" title="Let an agent use it">
        <p className="mb-4 text-[15px] leading-relaxed text-steel">
          Claude, Codex, and OpenCode can use the same moves. The skill is in
          the repo.
        </p>
        <Command
          text={`curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install-agent-skill.sh | bash`}
        />
        <p className="mt-4 text-[15px] leading-relaxed text-steel">
          Read the{" "}
          <a href={SKILL_URL} rel="noopener noreferrer" className="text-paper underline">
            skill
          </a>{" "}
          or the{" "}
          <a href={AGENT_SKILL_INSTALL_URL} rel="noopener noreferrer" className="text-paper underline">
            installer
          </a>{" "}
          if you want to see what it puts on disk.
        </p>
      </Section>

      <Section id="recovery" kicker="09" title="Take it off a machine">
        <p className="mb-4 text-[15px] leading-relaxed text-steel">
          There is no uninstall script. Stop what is running, then delete the
          files. State lives in <span className="font-mono text-paper">{STATE_DIR}</span>.
        </p>
        <p className="mb-3 font-medium text-paper">Mac</p>
        <p className="mb-3 text-[15px] leading-relaxed text-steel">
          Kick the launch agents out first. Then delete the files.
        </p>
        <Command text={RECOVERY_MAC_BOOTOUT_MESH} />
        <Command text={RECOVERY_MAC_BOOTOUT_CUA} />
        <Command text={RECOVERY_MAC_BOOTOUT_MENUBAR} />
        <Command
          text={`rm -f ~/Library/LaunchAgents/com.bridgesessions.mesh.plist \\
  ~/Library/LaunchAgents/com.bridgesessions.cua-helper.plist \\
  ~/Library/LaunchAgents/com.minddragon.bridgesessions.menubar.plist`}
        />
        <Command
          text={`rm -f ~/.local/bin/${BINARY} ~/.local/bin/${CLI} \\
  ~/Library/Application\\ Support/BridgeSessions/bridge-menubar.app`}
        />
        <Command text={`rm -rf ${STATE_DIR}`} />
        <p className="mt-6 mb-3 font-medium text-paper">Linux</p>
        <Command text={RECOVERY_LINUX_STOP_DAEMON} />
        <Command
          text={`systemctl --user disable bridgesessions.service 2>/dev/null || true`}
        />
        <Command
          text={`rm -f ~/.config/systemd/user/bridgesessions.service \\
  ~/.config/autostart/bridgesessions.desktop \\
  ~/.local/bin/${BINARY} ~/.local/bin/${CLI}`}
        />
        <Command text={`rm -rf ${STATE_DIR}`} />
        <p className="mt-6 mb-3 font-medium text-paper">Windows</p>
        <p className="mb-3 text-[15px] leading-relaxed text-steel">
          Stop the tray and the helper before you delete anything.
        </p>
        <Command text={RECOVERY_WIN_END_MESH_TASK} />
        <Command text={RECOVERY_WIN_STOP_TRAY} />
        <Command text={RECOVERY_WIN_END_CUA_TASK} />
        <Command text={RECOVERY_WIN_KILL_HELPER} />
        <Command
          text={`schtasks /Delete /TN "BridgeSessions" /F
schtasks /Delete /TN "BridgeSessions-CuaHelper" /F`}
        />
        <Command
          text={`Remove-Item -Recurse -Force "$env:LOCALAPPDATA\\bridgesessions"
Remove-Item -Recurse -Force "$env:USERPROFILE\\.bridgesessions"`}
        />
        <p className="mt-6 text-[15px] leading-relaxed text-steel">
          If a leftover process will not die, say so on{" "}
          <a href={ISSUES_URL} rel="noopener noreferrer" className="text-paper underline">
            GitHub issues
          </a>
          . Do not invent a cleaner script and call it ours.
        </p>
      </Section>
    </div>
  );
}
