import Link from "next/link";
import {
  BETA_BOUNDARY,
  LICENSE_LINE,
  LICENSE_URL,
  NAV,
  PRODUCT,
  REPO_URL,
  SECURITY_MD_URL,
  TLS_PROFILE,
  TRANSPORT,
  VERSION,
} from "@/lib/product";

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
      <div className="mx-auto flex max-w-6xl items-center justify-between gap-3 px-4 py-3 sm:h-14 sm:px-6 sm:py-0">
        <Link href="/" className="shrink-0 font-medium tracking-tight text-paper">
          {PRODUCT}
        </Link>
        <details className="relative sm:hidden">
          <summary className="cursor-pointer list-none font-mono text-[11px] tracking-[0.12em] text-steel uppercase [&::-webkit-details-marker]:hidden">
            Menu
          </summary>
          <nav
            aria-label="Mobile"
            className="absolute right-0 z-50 mt-2 w-56 rounded-[3px] border border-border bg-panel p-2"
          >
            {NAV.map((item) => (
              <Link
                key={item.href}
                href={item.href}
                className="block px-2 py-2 font-mono text-[11px] tracking-[0.12em] text-steel uppercase hover:text-paper"
              >
                {item.label}
              </Link>
            ))}
          </nav>
        </details>
        <nav
          aria-label="Primary"
          className="hidden min-w-0 items-center gap-4 overflow-x-auto font-mono text-[11px] tracking-[0.12em] text-steel uppercase sm:flex"
        >
          {NAV.map((item) => (
            <Link key={item.href} href={item.href} className="hover:text-paper">
              {item.label}
            </Link>
          ))}
        </nav>
        <Link
          href="/install"
          className="shrink-0 font-mono text-[11px] tracking-[0.12em] text-signal uppercase"
        >
          Try the release
        </Link>
      </div>
    </header>
  );
}

export function LiveLine() {
  return (
    <div className="border-b border-border bg-panel">
      <p className="mx-auto flex max-w-6xl items-center gap-x-6 overflow-x-auto px-4 py-2 font-mono text-[11px] tracking-[0.14em] text-steel uppercase sm:px-6">
        <span className="inline-flex shrink-0 items-center gap-2 text-signal">
          <span className="live-dot" aria-hidden />
          live line {VERSION}
        </span>
        <span className="shrink-0">{TRANSPORT}</span>
        <span className="shrink-0">{TLS_PROFILE}</span>
      </p>
    </div>
  );
}

export function TrustStrip() {
  return (
    <div className="border-b border-border">
      <div className="mx-auto flex max-w-6xl flex-col gap-2 px-4 py-3 text-[12px] leading-relaxed text-steel sm:px-6 sm:text-[13px]">
        <p>
          Primary repo:{" "}
          <a href={REPO_URL} rel="noopener noreferrer" className="break-all text-paper">
            {REPO_URL}
          </a>
        </p>
        <p>
          <a href={LICENSE_URL} rel="noopener noreferrer" className="text-paper">
            {LICENSE_LINE}
          </a>
        </p>
        <p>
          Public beta. {BETA_BOUNDARY}{" "}
          <a href={SECURITY_MD_URL} rel="noopener noreferrer" className="text-paper">
            SECURITY.md
          </a>
        </p>
      </div>
    </div>
  );
}

export function SiteFooter() {
  return (
    <footer className="border-t border-border">
      <div className="mx-auto max-w-6xl space-y-3 px-4 py-6 text-[12px] leading-relaxed text-steel sm:px-6">
        <p className="font-mono text-[11px] tracking-[0.12em] uppercase">
          {PRODUCT} · {VERSION} · public beta · marketing source in this GitHub repo · not a production cutover
        </p>
        <p>
          Repo:{" "}
          <a href={REPO_URL} rel="noopener noreferrer" className="break-all text-paper">
            {REPO_URL}
          </a>
        </p>
        <p>
          <a href={LICENSE_URL} rel="noopener noreferrer" className="text-paper">
            {LICENSE_LINE}
          </a>
        </p>
        <p>
          {BETA_BOUNDARY}{" "}
          <a href={SECURITY_MD_URL} rel="noopener noreferrer" className="text-paper">
            Read SECURITY.md
          </a>
        </p>
      </div>
    </footer>
  );
}
