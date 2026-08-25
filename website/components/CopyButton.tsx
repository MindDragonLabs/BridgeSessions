"use client";

import { useState } from "react";

export function CopyButton({ text }: { text: string }) {
  const [copied, setCopied] = useState(false);

  async function copy() {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1600);
    } catch {
      setCopied(false);
    }
  }

  return (
    <button
      type="button"
      onClick={copy}
      className="absolute top-2 right-2 rounded-[3px] border border-border bg-ink/80 px-2 py-1 font-mono text-[10px] tracking-[0.14em] text-steel uppercase hover:border-signal hover:text-signal"
    >
      {copied ? "Copied" : "Copy"}
    </button>
  );
}
