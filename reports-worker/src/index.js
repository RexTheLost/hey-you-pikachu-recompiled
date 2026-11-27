// Crash / exit report inbox for the Hey You, Pikachu Android port.
// The app sends a report on its next start after a crash or an abnormal exit; tools/fetch_reports.py
// downloads waiting ones into reports/ and marks them fetched.
//
//   POST /report  {"kind", "version", "device", "text"}   header X-HYP-App: hyp-android
//        -> {"ok": true, "id": N}
//   GET  /reports?key=ADMIN_KEY            -> {"reports": [...]} not fetched yet
//   POST /reports/N/fetched?key=ADMIN_KEY  -> marks one fetched
//
// Live log (switched on in the game's Words list; tools/live_log.py follows it):
//   POST /live   {"session", "text"}                      header X-HYP-App: hyp-android
//   POST /clip?session=S&name=N   body: a WAV of one Talk recording
//   GET  /live?key=ADMIN_KEY&after=ID   -> {"lines": [{id, at, session, text}], "clips": [{id, at, session, name, size}]}
//   GET  /clip/ID?key=ADMIN_KEY         -> the WAV
//
// Updates (tools/publish_android.py puts the files in the BUILDS KV namespace):
//   GET  /latest           -> {"versionCode", "versionName", "sha256", "size", "notes"} or 404
//   GET  /apk/<versionCode> -> the APK
//
// Deploy from this folder: npx wrangler deploy. Secret: ADMIN_KEY.
import { DurableObject } from "cloudflare:workers";

const MAX_TEXT = 60000;        // characters per report (crash report + log tails)
const PER_SENDER_HOUR = 10;    // reports one address may send in an hour
const ALL_HOUR = 200;          // reports the inbox takes in an hour, from everyone
const KEEP_DAYS = 90;          // fetched reports are deleted after this
const LIVE_PER_MINUTE = 40;    // live log + clip posts one address may make in a minute
const LIVE_MAX_TEXT = 20000;   // characters per live log post
const CLIP_MAX = 400000;       // bytes per clip (about 12 s of 16 kHz mono 16-bit)
const LIVE_KEEP_DAYS = 7;      // live logs and clips are deleted after this

export default {
	async fetch(request, env) {
		const url = new URL(request.url);
		if (url.pathname === "/") {
			return new Response("Hey You, Pikachu Android report inbox.\n");
		}
		if (url.pathname === "/latest" && request.method === "GET") {
			const latest = await env.BUILDS.get("latest");
			if (!latest) {
				return new Response("No build published yet", { status: 404 });
			}
			return new Response(latest, { headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
		}
		const apk = url.pathname.match(/^\/apk\/(\d+)$/);
		if (apk && request.method === "GET") {
			const body = await env.BUILDS.get("apk:" + apk[1], { type: "stream" });
			if (!body) {
				return new Response("Not found", { status: 404 });
			}
			return new Response(body, { headers: {
				"Content-Type": "application/vnd.android.package-archive",
				"Content-Disposition": `attachment; filename="HeyYouPikachu-${apk[1]}.apk"`,
			} });
		}
		// VR (Quest) update channel: tools/vr_release.py publish puts "vr-latest" and "vr-apk:<code>" in BUILDS.
		if (url.pathname === "/vr/latest" && request.method === "GET") {
			const latest = await env.BUILDS.get("vr-latest");
			if (!latest) {
				return new Response("No VR build published yet", { status: 404 });
			}
			return new Response(latest, { headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
		}
		const vrApk = url.pathname.match(/^\/vr\/apk\/(\d+)$/);
		if (vrApk && request.method === "GET") {
			const body = await env.BUILDS.get("vr-apk:" + vrApk[1], { type: "stream" });
			if (!body) {
				return new Response("Not found", { status: 404 });
			}
			return new Response(body, { headers: {
				"Content-Type": "application/vnd.android.package-archive",
				"Content-Disposition": `attachment; filename="HeyYouPikachuVR-${vrApk[1]}.apk"`,
			} });
		}
		// One-time link for a personal build (tools/vr_release.py first-link / new-link): the token's hash is a
		// BUILDS key naming the blob; the first download deletes the token, so the link works once.
		const first = url.pathname.match(/^\/vr\/first\/([A-Za-z0-9_-]{20,80})$/);
		if (first && request.method === "GET") {
			const tokenKey = "vr-first-token:" + await hash(first[1]);
			const blobKey = await env.BUILDS.get(tokenKey);
			if (!blobKey) {
				return new Response("This link was already used or has expired. Ask for a new one.\n", { status: 410 });
			}
			await env.BUILDS.delete(tokenKey);
			const body = await env.BUILDS.get(blobKey, { type: "stream" });
			if (!body) {
				return new Response("This build is gone. Ask for a new link.\n", { status: 410 });
			}
			return new Response(body, { headers: {
				"Content-Type": "application/vnd.android.package-archive",
				"Content-Disposition": `attachment; filename="HeyYouPikachuVR.apk"`,
				"Cache-Control": "no-store",
			} });
		}
		if ((url.pathname === "/live" || url.pathname === "/clip") && request.method === "POST") {
			if (request.headers.get("X-HYP-App") !== "hyp-android") {
				return new Response("Not found", { status: 404 });
			}
			const sender = await hash(request.headers.get("CF-Connecting-IP") || "unknown");
			const inbox = env.REPORTS.getByName("inbox");
			let result;
			if (url.pathname === "/live") {
				let body;
				try {
					body = await request.json();
				} catch (e) {
					return Response.json({ ok: false, error: "bad body" }, { status: 400 });
				}
				result = await inbox.addLive(clean(body.session, 40), clean(body.text, LIVE_MAX_TEXT), sender);
			} else {
				const bytes = new Uint8Array(await request.arrayBuffer());
				if (bytes.length === 0 || bytes.length > CLIP_MAX) {
					return Response.json({ ok: false, error: "bad clip size" }, { status: 400 });
				}
				result = await inbox.addClip(clean(url.searchParams.get("session"), 40), clean(url.searchParams.get("name"), 120), bytes, sender);
			}
			return Response.json(result, { status: result.ok ? 200 : (result.status || 400) });
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
		if (url.pathname === "/live" && request.method === "GET") {
			if (!env.ADMIN_KEY || url.searchParams.get("key") !== env.ADMIN_KEY) {
				return new Response("Not found", { status: 404 });
			}
			const after = Number(url.searchParams.get("after") || 0);
			return Response.json(await env.REPORTS.getByName("inbox").liveAfter(after));
		}
		const clip = url.pathname.match(/^\/clip\/(\d+)$/);
		if (clip && request.method === "GET") {
			if (!env.ADMIN_KEY || url.searchParams.get("key") !== env.ADMIN_KEY) {
				return new Response("Not found", { status: 404 });
			}
			const data = await env.REPORTS.getByName("inbox").clipData(Number(clip[1]));
			if (!data) {
				return new Response("Not found", { status: 404 });
			}
			return new Response(data, { headers: { "Content-Type": "audio/wav" } });
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
			this.ctx.storage.sql.exec(`CREATE TABLE IF NOT EXISTS live (
				id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, session TEXT NOT NULL,
				kind TEXT NOT NULL, name TEXT NOT NULL, text TEXT NOT NULL, data BLOB, sender TEXT NOT NULL)`);
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

	liveAllowed(sender) {
		const minuteAgo = Date.now() - 60 * 1000;
		const n = this.ctx.storage.sql.exec("SELECT COUNT(*) AS n FROM live WHERE sender = ? AND at > ?", sender, minuteAgo).one().n;
		return n < LIVE_PER_MINUTE;
	}

	async addLive(session, text, sender) {
		if (!text) return { ok: true };
		if (!this.liveAllowed(sender)) return { ok: false, status: 429, error: "slow down" };
		const now = Date.now();
		this.ctx.storage.sql.exec("DELETE FROM live WHERE at < ?", now - LIVE_KEEP_DAYS * 86400 * 1000);
		const id = this.ctx.storage.sql.exec("INSERT INTO live (at, session, kind, name, text, data, sender) VALUES (?, ?, 'log', '', ?, NULL, ?) RETURNING id",
			now, session, text, sender).one().id;
		return { ok: true, id };
	}

	async addClip(session, name, bytes, sender) {
		if (!this.liveAllowed(sender)) return { ok: false, status: 429, error: "slow down" };
		const id = this.ctx.storage.sql.exec("INSERT INTO live (at, session, kind, name, text, data, sender) VALUES (?, ?, 'clip', ?, '', ?, ?) RETURNING id",
			Date.now(), session, name, bytes.buffer, sender).one().id;
		return { ok: true, id };
	}

	async liveAfter(after) {
		const rows = this.ctx.storage.sql.exec("SELECT id, at, session, kind, name, text, length(data) AS size FROM live WHERE id > ? ORDER BY id LIMIT 500", after).toArray();
		return {
			lines: rows.filter((r) => r.kind === "log").map((r) => ({ id: r.id, at: r.at, session: r.session, text: r.text })),
			clips: rows.filter((r) => r.kind === "clip").map((r) => ({ id: r.id, at: r.at, session: r.session, name: r.name, size: r.size })),
		};
	}

	async clipData(id) {
		const rows = this.ctx.storage.sql.exec("SELECT data FROM live WHERE id = ? AND kind = 'clip'", id).toArray();
		return rows.length ? rows[0].data : null;
	}

	async waiting() {
		return this.ctx.storage.sql.exec("SELECT id, at, kind, version, device, text FROM reports WHERE fetched = 0 ORDER BY id").toArray();
	}

	async markFetched(id) {
		this.ctx.storage.sql.exec("UPDATE reports SET fetched = 1 WHERE id = ?", id);
	}
}
