import Link from "next/link";
import { NAV, PRODUCT, REPO_URL, VERSION } from "@/lib/product";

export function SkipLink() {
  return (
    <a
      href="#content"
      className="sr-only focus:not-sr-only focus:absolute focus:top-2 focus:left-2 focus:z-50 focus:bg-signal focus:px-3 focus:py-2 focus:font-mono focus:text-xs focus:text-signal-ink"
    >
      Skip to content
    </a>
  );
}

export function SiteHeader() {
  return (
    <header className="sticky top-0 z-40 border-b border-border bg-ink/85 backdrop-blur-md">
      <div className="mx-auto flex max-w-3xl items-center justify-between gap-3 px-4 py-3 sm:h-14 sm:px-6 sm:py-0">
        <Link href="/" className="shrink-0 font-medium tracking-tight text-paper">
          {PRODUCT}
        </Link>
        <nav
          aria-label="Site"
          className="flex items-center gap-5 font-mono text-[11px] tracking-[0.12em] text-steel uppercase"
        >
          {NAV.map((item) => (
            <Link key={item.href} href={item.href} className="hover:text-paper">
              {item.label}
            </Link>
          ))}
          <a href={REPO_URL} rel="noopener noreferrer" className="hover:text-paper">
            GitHub
          </a>
        </nav>
      </div>
    </header>
  );
}

export function SiteFooter() {
  return (
    <footer className="border-t border-border">
      <div className="mx-auto flex max-w-3xl flex-wrap items-center justify-between gap-3 px-4 py-6 text-[12px] text-steel sm:px-6">
        <p>
          {PRODUCT}{" "}
          <span className="font-mono text-[11px] text-mute">{VERSION}</span>
        </p>
        <a href={REPO_URL} rel="noopener noreferrer" className="text-paper hover:underline">
          GitHub
        </a>
      </div>
    </footer>
  );
}
