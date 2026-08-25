import Link from "next/link";

export default function NotFound() {
  return (
    <section className="mx-auto max-w-6xl px-4 py-24 sm:px-6">
      <p className="font-mono text-[11px] tracking-[0.2em] text-steel uppercase">
        404
      </p>
      <h1 className="font-display mt-4 text-4xl text-paper">
        This path does not exist.
      </h1>
      <p className="mt-4 max-w-lg text-sm text-steel">
        This site has a homepage and /install. Operator docs are in the GitHub
        repository.
      </p>
      <p className="mt-8">
        <Link
          href="/install"
          className="font-mono text-[12px] tracking-[0.12em] text-signal uppercase"
        >
          Install
        </Link>
      </p>
    </section>
  );
}
