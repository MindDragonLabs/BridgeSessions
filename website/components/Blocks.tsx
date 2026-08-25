import type { ReactNode } from "react";
import { CopyButton } from "./CopyButton";

export function Eyebrow({ children }: { children: ReactNode }) {
  return (
    <p className="font-mono text-[11px] tracking-[0.2em] text-steel uppercase">
      {children}
    </p>
  );
}

export function Section({
  id,
  children,
  className = "",
}: {
  id?: string;
  children: ReactNode;
  className?: string;
}) {
  return (
    <section
      id={id}
      className={`scroll-mt-24 border-b border-border ${className}`}
    >
      <div className="mx-auto max-w-6xl px-4 py-14 sm:px-6 sm:py-16">
        {children}
      </div>
    </section>
  );
}

export function Command({
  code,
  caption,
}: {
  code: string;
  caption?: string;
}) {
  return (
    <figure className="overflow-hidden rounded-[3px] border border-border bg-ink">
      {caption ? (
        <figcaption className="border-b border-border px-3 py-2 font-mono text-[11px] tracking-[0.14em] text-steel uppercase">
          {caption}
        </figcaption>
      ) : null}
      <div className="relative">
        <pre className="overflow-x-auto p-3 pr-16 font-mono text-[13px] leading-relaxed text-paper/90 sm:p-4">
          <code>{code}</code>
        </pre>
        <CopyButton text={code} />
      </div>
    </figure>
  );
}

export function ExtLink({
  href,
  children,
}: {
  href: string;
  children: ReactNode;
}) {
  return (
    <a
      href={href}
      className="text-signal underline decoration-signal/30 underline-offset-4 hover:decoration-signal"
      rel="noopener noreferrer"
    >
      {children}
    </a>
  );
}

export function Card({
  title,
  children,
  kicker,
}: {
  title: string;
  children: ReactNode;
  kicker?: string;
}) {
  return (
    <article className="rounded-[3px] border border-border bg-panel p-4 sm:p-5">
      {kicker ? (
        <p className="font-mono text-[11px] tracking-[0.16em] text-steel uppercase">
          {kicker}
        </p>
      ) : null}
      <h3 className="mt-1 text-lg text-paper">{title}</h3>
      <div className="mt-2 text-sm leading-relaxed text-paper/80">{children}</div>
    </article>
  );
}
