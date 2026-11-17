// Crash / exit report inbox for the Hey You, Pikachu Android port.
// The app sends a report on its next start after a crash or an abnormal exit; tools/fetch_reports.py
// downloads waiting ones into reports/ and marks them fetched.
//
//   POST /report  {"kind", "version", "device", "text"}   header X-HYP-App: hyp-android
//        -> {"ok": true, "id": N}
//   GET  /reports?key=ADMIN_KEY            -> {"reports": [...]} not fetched yet
//   POST /reports/N/fetched?key=ADMIN_KEY  -> marks one fetched
//
// Deploy from this folder: npx wrangler deploy. Secret: ADMIN_KEY.
import { DurableObject } from "cloudflare:workers";

const MAX_TEXT = 60000;        // characters per report (crash report + log tails)
const PER_SENDER_HOUR = 10;    // reports one address may send in an hour
const ALL_HOUR = 200;          // reports the inbox takes in an hour, from everyone
const KEEP_DAYS = 90;          // fetched reports are deleted after this

export default {
	async fetch(request, env) {
		const url = new URL(request.url);
		if (url.pathname === "/") {
			return new Response("Hey You, Pikachu Android report inbox.\n");
		}
		if (url.pathname === "/report" && request.method === "POST") {
			if (request.headers.get("X-HYP-App") !== "hyp-android") {
				return new Response("Not found", { status: 404 });
			}
			let report;
			try {
				report = await request.json();
			} catch (e) {
				return Response.json({ ok: false, error: "That was not a report." }, { status: 400 });
			}
			const sender = await hash(request.headers.get("CF-Connecting-IP") || "unknown");
			const result = await env.REPORTS.getByName("inbox").add(report, sender);
			return Response.json(result, { status: result.ok ? 200 : (result.status || 400) });
		}
		if (url.pathname === "/reports" || url.pathname.startsWith("/reports/")) {
			if (!env.ADMIN_KEY || url.searchParams.get("key") !== env.ADMIN_KEY) {
				return new Response("Not found", { status: 404 });
			}
			const inbox = env.REPORTS.getByName("inbox");
			if (url.pathname === "/reports" && request.method === "GET") {
				return Response.json({ reports: await inbox.waiting() });
			}
			const m = url.pathname.match(/^\/reports\/(\d+)\/fetched$/);
			if (m && request.method === "POST") {
				await inbox.markFetched(Number(m[1]));
				return Response.json({ ok: true });
			}
		}
		return new Response("Not found", { status: 404 });
	},
};

async function hash(text) {
	// Only a hash of the sender's address is kept, for the hourly limit.
	const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode("hyp-reports:" + text));
	return [...new Uint8Array(digest)].slice(0, 12).map((b) => b.toString(16).padStart(2, "0")).join("");
}

function clean(value, limit) {
	return String(value ?? "").replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/g, "").slice(0, limit);
}

export class Reports extends DurableObject {
	constructor(ctx, env) {
		super(ctx, env);
		ctx.blockConcurrencyWhile(async () => {
			this.ctx.storage.sql.exec(`CREATE TABLE IF NOT EXISTS reports (
				id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, kind TEXT NOT NULL,
				version TEXT NOT NULL, device TEXT NOT NULL, text TEXT NOT NULL,
				sender TEXT NOT NULL, fetched INTEGER NOT NULL DEFAULT 0)`);
		});
	}

	async add(report, sender) {
		const kind = clean(report && report.kind, 40) || "report";
		const text = clean(report && report.text, MAX_TEXT);
		if (!text.trim()) {
			return { ok: false, error: "The report is empty." };
		}
		const now = Date.now();
		const hourAgo = now - 3600 * 1000;
		const mine = this.ctx.storage.sql.exec("SELECT COUNT(*) AS n FROM reports WHERE sender = ? AND at > ?", sender, hourAgo).one().n;
		const all = this.ctx.storage.sql.exec("SELECT COUNT(*) AS n FROM reports WHERE at > ?", hourAgo).one().n;
		if (mine >= PER_SENDER_HOUR || all >= ALL_HOUR) {
			return { ok: false, status: 429, error: "Too many reports in the last hour." };
		}
		this.ctx.storage.sql.exec("DELETE FROM reports WHERE fetched = 1 AND at < ?", now - KEEP_DAYS * 86400 * 1000);
		const id = this.ctx.storage.sql.exec(
			"INSERT INTO reports (at, kind, version, device, text, sender) VALUES (?, ?, ?, ?, ?, ?) RETURNING id",
			now, kind, clean(report.version, 60), clean(report.device, 120), text, sender).one().id;
		return { ok: true, id };
	}

	async waiting() {
		return this.ctx.storage.sql.exec("SELECT id, at, kind, version, device, text FROM reports WHERE fetched = 0 ORDER BY id").toArray();
	}

	async markFetched(id) {
		this.ctx.storage.sql.exec("UPDATE reports SET fetched = 1 WHERE id = ?", id);
	}
}
