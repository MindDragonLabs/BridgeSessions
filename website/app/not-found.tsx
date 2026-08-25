import Link from "next/link";

export default function NotFound() {
  return (
    <section className="mx-auto max-w-3xl px-4 py-24 sm:px-6">
      <p className="font-mono text-[11px] tracking-[0.14em] text-mute uppercase">
        404
      </p>
      <h1 className="mt-4 font-display text-4xl text-paper">
        That page is not here.
      </h1>
      <p className="mt-4 max-w-lg text-[15px] leading-relaxed text-steel">
        This site is a homepage and an install page. Everything else lives on
        GitHub.
      </p>
      <p className="mt-8">
        <Link href="/install" className="text-paper underline">
          Install
        </Link>
      </p>
    </section>
  );
}
