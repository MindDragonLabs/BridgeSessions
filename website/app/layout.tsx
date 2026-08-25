import type { Metadata } from "next";
import { IBM_Plex_Mono, IBM_Plex_Sans, Instrument_Serif } from "next/font/google";
import {
  LiveLine,
  SiteFooter,
  SiteHeader,
  SkipLink,
  TrustStrip,
} from "@/components/Chrome";
import { DESCRIPTION, PRODUCT, VERSION } from "@/lib/product";
import "./globals.css";

const plexSans = IBM_Plex_Sans({
  variable: "--font-plex-sans",
  subsets: ["latin"],
  weight: ["400", "500", "600"],
});

const plexMono = IBM_Plex_Mono({
  variable: "--font-plex-mono",
  subsets: ["latin"],
  weight: ["400", "500"],
});

const display = Instrument_Serif({
  variable: "--font-display-face",
  subsets: ["latin"],
  weight: "400",
});

export const metadata: Metadata = {
  title: {
    default: PRODUCT,
    absolute: `${PRODUCT} ${VERSION}`,
    template: "%s",
  },
  description: DESCRIPTION,
  applicationName: PRODUCT,
};

export default function RootLayout({ children }: LayoutProps<"/">) {
  return (
    <html
      lang="en"
      className={`dark ${plexSans.variable} ${plexMono.variable} ${display.variable} h-full`}
    >
      <body className="flex min-h-full flex-col bg-ink text-paper antialiased">
        <SkipLink />
        <SiteHeader />
        <LiveLine />
        <TrustStrip />
        <main id="content" className="flex-1">
          {children}
        </main>
        <SiteFooter />
      </body>
    </html>
  );
}
