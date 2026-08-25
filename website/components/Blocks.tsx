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
  title,
  kicker,
  children,
  className = "",
}: {
  id?: string;
  title?: string;
  kicker?: string;
  children: ReactNode;
  className?: string;
}) {
  return (
    <section id={id} className={`mt-16 scroll-mt-24 ${className}`}>
      {kicker ? (
        <p className="font-mono text-[11px] tracking-[0.12em] text-steel uppercase">
          {kicker}
        </p>
      ) : null}
      {title ? (
        <h2 className={`font-display text-3xl text-paper ${kicker ? "mt-2" : ""}`}>
          {title}
        </h2>
      ) : null}
      <div className={title || kicker ? "mt-5" : undefined}>{children}</div>
    </section>
  );
}

export function Command({
  code,
  text,
  caption,
}: {
  code?: string;
  text?: string;
  caption?: string;
}) {
  const value = code ?? text ?? "";
  return (
    <figure className="mt-3 overflow-hidden rounded-[3px] border border-border bg-panel">
      {caption ? (
        <figcaption className="border-b border-border px-3 py-2 font-mono text-[11px] tracking-[0.14em] text-steel uppercase">
          {caption}
        </figcaption>
      ) : null}
      <div className="relative">
        <pre className="overflow-x-auto p-3 pr-16 font-mono text-[13px] leading-relaxed text-paper/90 sm:p-4">
          <code>{value}</code>
        </pre>
        <CopyButton text={value} />
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
      className="text-paper underline underline-offset-4 hover:text-signal"
      rel="noopener noreferrer"
    >
      {children}
    </a>
  );
}
