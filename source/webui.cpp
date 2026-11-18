// The page the Wi-Fi transfer server sends to browsers: a file manager for the Switch's drives.
extern const char* const kWebUi;
const char* const kWebUi = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>SwitchMX Transfer</title>
<style>
:root{--bg:#0a0b12;--panel:#11131d;--card:#161927;--card2:#1c2032;--hover:#ffffff0a;--line:#262a40;--line2:#30354f;--text:#eef0f8;--dim:#a6abc6;--faint:#6c7192;
--accent:#7c6bff;--accent2:#22d3ee;--pink:#f472b6;--danger:#ff5d73;--ok:#34d399;--warn:#fbbf24;--r:14px;--side:268px}
*{box-sizing:border-box}html,body{margin:0;height:100%}[hidden]{display:none!important}
body{background:var(--bg);color:var(--text);font:14.5px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-font-smoothing:antialiased;overflow:hidden}
button,input,select{font:inherit;color:inherit}button{cursor:pointer;background:none;border:0;padding:0}
svg.i{width:18px;height:18px;flex:none}
.app{display:grid;grid-template-columns:var(--side) 1fr;height:100vh}
aside{background:linear-gradient(180deg,#121426,#0d0f19);border-right:1px solid var(--line);display:flex;flex-direction:column;min-height:0}
.brand{display:flex;align-items:center;gap:12px;padding:20px 18px 14px}
.logo{width:40px;height:40px;border-radius:12px;background:linear-gradient(135deg,#241b5c,#0b091e);display:grid;place-items:center;box-shadow:0 6px 18px #0007,inset 0 0 0 1px #ffffff14;flex:none}
.srcnote{padding:0 18px 16px;color:var(--faint);font-size:11px;line-height:1.5}
.srcnote a{color:var(--accent);text-decoration:none}.srcnote a:hover{text-decoration:underline}
.logo svg{width:26px;height:26px}.brand b{font-size:16px;display:block}.brand small{color:var(--faint);font-size:12px}
.sect{padding:10px 18px 6px;color:var(--faint);font-size:11px;font-weight:700;letter-spacing:.08em;text-transform:uppercase}
.drives{padding:0 10px;display:flex;flex-direction:column;gap:6px;overflow:auto}
.drive{display:flex;gap:12px;align-items:center;padding:11px 12px;border-radius:12px;text-align:left;width:100%;border:1px solid transparent}
.drive:hover{background:var(--hover)}.drive.on{background:linear-gradient(180deg,#7c6bff26,#7c6bff0d);border-color:#7c6bff55}
.drive .di{width:34px;height:34px;border-radius:10px;display:grid;place-items:center;background:#ffffff0d;flex:none}
.drive b{display:block;font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.drive small{color:var(--faint);font-size:12px}
.meter{height:5px;border-radius:3px;background:#ffffff12;margin-top:6px;overflow:hidden}.meter i{display:block;height:100%;border-radius:3px;background:linear-gradient(90deg,var(--accent2),var(--accent))}
.meter i.full{background:linear-gradient(90deg,var(--warn),var(--danger))}
.sidefoot{margin-top:auto;padding:14px 18px;color:var(--faint);font-size:12px;border-top:1px solid var(--line);display:flex;align-items:center;gap:8px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--ok);box-shadow:0 0 0 4px #34d39922}
main{display:flex;flex-direction:column;min-width:0;min-height:0}
.top{display:flex;align-items:center;gap:10px;padding:14px 20px;border-bottom:1px solid var(--line);background:#0b0c15cc;backdrop-filter:blur(10px)}
.menu-btn{display:none!important}
.crumbs{display:flex;align-items:center;gap:2px;flex:1;min-width:0;overflow:hidden}
.crumbs button{padding:6px 9px;border-radius:8px;color:var(--dim);white-space:nowrap;max-width:240px;overflow:hidden;text-overflow:ellipsis}
.crumbs button:hover{background:var(--hover);color:var(--text)}.crumbs button:last-child{color:var(--text);font-weight:650}
.crumbs .sep{color:var(--faint);flex:none}
.search{position:relative;width:min(300px,30vw)}.search input{width:100%;padding:9px 12px 9px 36px;border-radius:11px;border:1px solid var(--line);background:var(--card);outline:none}
.search input:focus{border-color:var(--accent);box-shadow:0 0 0 3px #7c6bff26}.search svg{position:absolute;left:11px;top:50%;transform:translateY(-50%);color:var(--faint)}
.btn{display:inline-flex;align-items:center;gap:8px;padding:9px 13px;border-radius:11px;border:1px solid var(--line);background:var(--card2);font-weight:600;font-size:13.5px;white-space:nowrap;text-decoration:none}
.btn:hover{border-color:var(--line2);background:#22273c}.btn.primary{background:linear-gradient(135deg,var(--accent),#5b8cff);border-color:transparent;box-shadow:0 6px 18px #7c6bff40}
.btn.primary:hover{filter:brightness(1.08)}.btn.danger{color:#ffd2d8;border-color:#ff5d7340;background:#ff5d7314}.btn.danger:hover{background:#ff5d7326}
.iconbtn{width:36px;height:36px;border-radius:10px;display:grid;place-items:center;color:var(--dim);flex:none}.iconbtn:hover{background:var(--hover);color:var(--text)}
.iconbtn.on{background:#7c6bff22;color:#c9c2ff}
.seg{display:flex;background:var(--card);border:1px solid var(--line);border-radius:11px;padding:3px}.seg .iconbtn{width:32px;height:30px;border-radius:8px}
.toolbar{display:flex;align-items:center;gap:8px;padding:12px 20px;flex-wrap:wrap}
.toolbar .info{color:var(--faint);font-size:13px;margin-right:auto}
select.sort{background:var(--card);border:1px solid var(--line);border-radius:11px;padding:8px 10px;outline:none}
.selbar{display:flex;align-items:center;gap:8px;padding:10px 20px;background:linear-gradient(90deg,#7c6bff1f,#22d3ee12);border-bottom:1px solid #7c6bff33;flex-wrap:wrap}
.selbar b{margin-right:auto}
.content{flex:1;overflow:auto;padding:0 20px 120px}
.list{border:1px solid var(--line);border-radius:var(--r);overflow:hidden;background:var(--panel)}
.row{display:grid;grid-template-columns:40px 44px minmax(0,1fr) 110px 170px 44px;align-items:center;gap:6px;padding:6px 10px;border-top:1px solid #ffffff07;cursor:default;user-select:none}
.row:first-child{border-top:0}.row:hover{background:var(--hover)}.row.sel{background:#7c6bff1c}
.row.head{position:sticky;top:0;z-index:2;background:#121522;color:var(--faint);font-size:11.5px;font-weight:700;text-transform:uppercase;letter-spacing:.06em}
.row.head button{color:inherit;text-transform:inherit;letter-spacing:inherit;font-weight:inherit;text-align:left}
.chk{width:20px;height:20px;border-radius:6px;border:1.5px solid var(--line2);display:grid;place-items:center;margin:auto;cursor:pointer}
.sel .chk,.chk.on{background:var(--accent);border-color:var(--accent)}.chk svg{width:13px;height:13px;color:#fff;opacity:0}.sel .chk svg,.chk.on svg{opacity:1}
.ico{width:36px;height:36px;border-radius:10px;display:grid;place-items:center;color:#fff;overflow:hidden}
.ico img{width:100%;height:100%;object-fit:cover}
.nm{min-width:0}.nm .t{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:520}.nm .p{color:var(--faint);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.meta{color:var(--dim);font-size:13px;white-space:nowrap}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:12px;align-items:start}
.tile{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);overflow:hidden;position:relative;user-select:none}
.tile:hover{border-color:var(--line2)}.tile.sel{border-color:var(--accent);box-shadow:0 0 0 2px #7c6bff55}
.tile .th{aspect-ratio:4/3;display:grid;place-items:center;color:#fff;position:relative;overflow:hidden}.tile .th img{position:absolute;inset:0;width:100%;height:100%;object-fit:cover}
.tile .th svg{width:44px;height:44px}.tile .cap{padding:9px 11px}.tile .cap .t{font-size:13px;font-weight:550;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.tile .cap small{color:var(--faint);font-size:12px}.tile .chk{position:absolute;top:9px;left:9px;background:#000a;border-color:#fff8}
.tile .more{position:absolute;top:6px;right:6px;background:#0009}
.empty{padding:80px 20px;text-align:center;color:var(--faint)}.empty svg{width:56px;height:56px;color:var(--line2);margin-bottom:10px}
.empty b{display:block;color:var(--dim);font-size:16px;margin-bottom:4px}
.drop{position:fixed;inset:14px;border:3px dashed var(--accent);border-radius:22px;background:#0a0b12e6;display:none;place-items:center;z-index:60;font-size:22px;font-weight:700;text-align:center}
.drop.on{display:grid}.drop small{display:block;color:var(--dim);font-size:14px;font-weight:500;margin-top:6px}
.ctx{position:fixed;z-index:70;min-width:200px;background:var(--card2);border:1px solid var(--line2);border-radius:13px;padding:6px;box-shadow:0 20px 50px #000b}
.ctx button{display:flex;align-items:center;gap:11px;width:100%;padding:9px 11px;border-radius:8px;text-align:left;font-size:14px}
.ctx button:hover{background:var(--hover)}.ctx button.danger{color:#ff9aa8}.ctx .tick{margin-left:auto;width:18px;height:18px;border-radius:5px;border:1.5px solid var(--line2);display:grid;place-items:center}
.ctx .tick.on{background:var(--accent);border-color:var(--accent)}.ctx .tick svg{width:12px;height:12px;color:#fff;opacity:0}.ctx .tick.on svg{opacity:1}
.ctx .lbl{padding:8px 11px 4px;color:var(--faint);font-size:11px;font-weight:700;letter-spacing:.07em;text-transform:uppercase}
.kv{display:grid;grid-template-columns:110px 1fr;gap:8px 12px;margin:6px 0 4px;font-size:14px}.kv span:nth-child(odd){color:var(--faint)}.kv span:nth-child(even){word-break:break-all}.ctx hr{border:0;border-top:1px solid var(--line);margin:5px 4px}
.ups{position:fixed;right:18px;bottom:18px;width:min(400px,calc(100vw - 36px));background:var(--card2);border:1px solid var(--line2);border-radius:16px;box-shadow:0 24px 60px #000b;z-index:50;overflow:hidden}
.ups .hd{display:flex;align-items:center;gap:10px;padding:12px 14px;border-bottom:1px solid var(--line)}.ups .hd b{flex:1}
.ups .total{height:3px;background:#ffffff10}.ups .total i{display:block;height:100%;background:linear-gradient(90deg,var(--accent2),var(--accent));transition:width .2s}
.ups .bd{max-height:44vh;overflow:auto}.up{display:grid;grid-template-columns:1fr auto;gap:2px 10px;padding:10px 14px;border-top:1px solid #ffffff08;align-items:center}
.up .n{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:13px}.up .s{color:var(--faint);font-size:12px;grid-column:1/3}
.up .bar{grid-column:1/3;height:4px;border-radius:2px;background:#ffffff10;overflow:hidden}.up .bar i{display:block;height:100%;width:0;background:var(--accent);transition:width .2s}
.up.done .bar i{background:var(--ok)}.up.err .bar i{background:var(--danger)}.up.err .s{color:#ff9aa8}
.modal{position:fixed;inset:0;background:#05060bcc;backdrop-filter:blur(6px);z-index:80;display:grid;place-items:center;padding:20px}
.dlg{background:var(--card2);border:1px solid var(--line2);border-radius:20px;width:min(460px,100%);padding:22px;box-shadow:0 30px 80px #000c}
.dlg h3{margin:0 0 6px;font-size:18px}.dlg p{margin:0 0 14px;color:var(--dim)}
.dlg input{width:100%;padding:11px 13px;border-radius:12px;border:1px solid var(--line);background:var(--bg);outline:none}.dlg input:focus{border-color:var(--accent)}
.dlg .acts{display:flex;gap:8px;justify-content:flex-end;margin-top:18px}
.picker{width:min(560px,100%);height:min(560px,90vh);display:flex;flex-direction:column;padding:0}
.picker .ph{padding:18px 20px 10px}.picker .pc{padding:0 16px 10px;color:var(--dim);line-height:2}
.picker .pc button{padding:3px 7px;border-radius:7px}.picker .pc button:hover{background:var(--hover)}
.picker .pl{flex:1;overflow:auto;border-top:1px solid var(--line);border-bottom:1px solid var(--line)}
.picker .pl button{display:flex;align-items:center;gap:12px;width:100%;padding:9px 20px;text-align:left}.picker .pl button:hover{background:var(--hover)}
.picker .pf{display:flex;gap:8px;justify-content:flex-end;padding:14px 20px}
.viewer{position:fixed;inset:0;z-index:90;background:#050509;display:flex;flex-direction:column}
.viewer .vh{display:flex;align-items:center;gap:10px;padding:12px 16px;color:#fff}.viewer .vh b{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.viewer .vb{flex:1;display:grid;place-items:center;min-height:0;position:relative;padding:0 16px 16px}
.viewer img,.viewer video{max-width:100%;max-height:100%;object-fit:contain;border-radius:8px}.viewer audio{width:min(560px,90vw)}
.viewer pre{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:18px;width:min(900px,94vw);max-height:100%;overflow:auto;white-space:pre-wrap;margin:0;font-size:13px}
.viewer .nav{position:absolute;top:50%;transform:translateY(-50%);width:52px;height:52px;border-radius:50%;background:#ffffff14;display:grid;place-items:center;color:#fff}
.viewer .nav:hover{background:#ffffff26}.viewer .nav.l{left:16px}.viewer .nav.r{right:16px}
.pin{position:fixed;inset:0;z-index:100;display:grid;place-items:center;background:radial-gradient(900px 500px at 70% 0,#2a1f63,transparent 60%),var(--bg)}
.pinbox{background:var(--card);border:1px solid var(--line);border-radius:24px;padding:32px 26px;width:min(380px,calc(100vw - 32px));text-align:center;box-shadow:0 30px 80px #000a}
.pinbox .logo{margin:0 auto;width:64px;height:64px;border-radius:18px}.pinbox .logo svg{width:42px;height:42px}
.pinbox h2{margin:16px 0 6px;font-size:20px}.pinbox p{color:var(--dim);margin:0 0 20px;font-size:14px}
.digits{display:flex;gap:10px;justify-content:center}.digits input{width:58px;height:66px;border-radius:15px;border:1px solid var(--line);background:var(--bg);font-size:28px;text-align:center;font-weight:700;outline:none}
.digits input:focus{border-color:var(--accent);box-shadow:0 0 0 3px #7c6bff33}.msg{color:var(--danger);min-height:20px;margin-top:14px;font-size:13px}
.toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%) translateY(20px);background:var(--card2);border:1px solid var(--line2);padding:11px 16px;border-radius:12px;z-index:110;
box-shadow:0 14px 40px #000a;opacity:0;transition:.2s;pointer-events:none;display:flex;gap:10px;align-items:center;max-width:90vw}.toast.on{opacity:1;transform:translateX(-50%)}
.toast.err{border-color:#ff5d7366}.spin{width:16px;height:16px;border-radius:50%;border:2px solid #ffffff30;border-top-color:var(--accent);animation:sp .8s linear infinite;flex:none;display:inline-block}
@keyframes sp{to{transform:rotate(360deg)}}
.mcrumbs{display:none;gap:2px;padding:10px 20px 0;overflow-x:auto;white-space:nowrap}.mcrumbs button{padding:5px 8px;border-radius:7px;color:var(--dim)}.mcrumbs button:last-child{color:var(--text);font-weight:650}
@media (max-width:860px){.app{grid-template-columns:1fr}aside{position:fixed;inset:0 auto 0 0;width:min(300px,85vw);z-index:75;transform:translateX(-102%);transition:.25s}
aside.open{transform:none;box-shadow:20px 0 60px #000c}.menu-btn{display:grid!important}.search{width:auto;flex:1}.crumbs{display:none}.mcrumbs{display:flex}
.row{grid-template-columns:36px 40px minmax(0,1fr) 40px}.row .c-size,.row .c-date{display:none}.toolbar .info{width:100%}.toolbar .btn span{display:none}}
</style></head><body>
<svg width="0" height="0" style="position:absolute"><defs>
<linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#3be0ff"/><stop offset=".6" stop-color="#8b6bff"/><stop offset="1" stop-color="#ff5fc8"/></linearGradient>
<symbol id="logo" viewBox="0 0 64 64"><path d="M22 14c0-3 3-4.6 5.5-3.1l24 14.5c2.6 1.6 2.6 5.3 0 6.9l-24 14.5C25 48.3 22 46.6 22 43.6z" fill="url(#g)"/><rect x="25" y="23" width="3" height="14" rx="1.5" fill="#120c30"/><rect x="30" y="20" width="3" height="20" rx="1.5" fill="#120c30"/><rect x="35" y="25" width="3" height="10" rx="1.5" fill="#120c30"/></symbol>
<symbol id="folder" viewBox="0 0 24 24"><path fill="currentColor" opacity=".55" d="M3 6.5A2.5 2.5 0 0 1 5.5 4h3.2a2 2 0 0 1 1.6.8L11.5 7H19a2 2 0 0 1 2 2v1H3z"/><rect x="3" y="9" width="18" height="11" rx="2.2" fill="currentColor"/></symbol>
<symbol id="film" viewBox="0 0 24 24"><rect x="3" y="4.5" width="18" height="15" rx="2.6" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M7 4.5v15M17 4.5v15" stroke="currentColor" stroke-width="1.8"/><path d="M10.3 9.2 14.6 12l-4.3 2.8z" fill="currentColor"/></symbol>
<symbol id="music" viewBox="0 0 24 24"><path d="M9.4 17.5V7M19.2 15.5V5" stroke="currentColor" stroke-width="1.8"/><path d="M8.6 5.6 20 3v3.8L8.6 9.4z" fill="currentColor"/><ellipse cx="6.8" cy="18" rx="3" ry="2.5" fill="currentColor"/><ellipse cx="16.6" cy="16" rx="3" ry="2.5" fill="currentColor"/></symbol>
<symbol id="image" viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="2.6" fill="none" stroke="currentColor" stroke-width="1.8"/><circle cx="8.5" cy="9" r="1.9" fill="currentColor"/><path d="M4.2 18.8V17L9 12.2l3.6 3.4 3-2.8 4.2 4v2z" fill="currentColor"/></symbol>
<symbol id="file" viewBox="0 0 24 24"><path d="M5 5a2 2 0 0 1 2-2h7l5 5v11a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2z" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M14 3v5h5" fill="none" stroke="currentColor" stroke-width="1.8"/></symbol>
<symbol id="sub" viewBox="0 0 24 24"><rect x="2.5" y="5" width="19" height="14" rx="3.2" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M6.5 11.2h2M11.4 11.2h6.1M6.5 15h6.1M15.4 15h2.1" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/></symbol>
<symbol id="zip" viewBox="0 0 24 24"><rect x="4" y="3" width="16" height="18" rx="2.4" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M12 3v3m0 2v2m0 2v2" stroke="currentColor" stroke-width="2"/><rect x="10" y="14" width="4" height="4" rx="1" fill="currentColor"/></symbol>
<symbol id="down" viewBox="0 0 24 24"><path d="M12 4v11m-5-5 5 5 5-5M5 20h14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="up" viewBox="0 0 24 24"><path d="M12 20V9m-5 5 5-5 5 5M5 4h14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="edit" viewBox="0 0 24 24"><path d="M4 20l.8-4.2L15.4 5.2l3.4 3.4L8.2 19.2z" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/></symbol>
<symbol id="trash" viewBox="0 0 24 24"><path d="M3.8 6.5h16.4M9 6.5V3.6h6v2.9M5.8 6.5l1.2 13.9h10l1.2-13.9M10 10.5v5.9M14 10.5v5.9" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="newf" viewBox="0 0 24 24"><path d="M3 18V6a1.5 1.5 0 0 1 1.5-1.5H9l2 2.5h8.5A1.5 1.5 0 0 1 21 8.5V18a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18zM12 10.2v5.6M9.2 13h5.6" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="refresh" viewBox="0 0 24 24"><path d="M20 12a8 8 0 1 1-2.3-5.6M20 4v4h-4" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="sd" viewBox="0 0 24 24"><path d="M8 3h11v18H5V6z" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/><path d="M9 6v3.8M12 6v3.8M15 6v3.8" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/></symbol>
<symbol id="usb" viewBox="0 0 24 24"><rect x="8.5" y="2.5" width="7" height="5.5" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.8"/><rect x="6" y="8" width="12" height="13.5" rx="2.6" fill="none" stroke="currentColor" stroke-width="1.8"/><circle cx="12" cy="17.2" r="1.2" fill="currentColor"/></symbol>
<symbol id="search" viewBox="0 0 24 24"><circle cx="10.5" cy="10.5" r="6.5" fill="none" stroke="currentColor" stroke-width="2"/><path d="M15.6 15.6 20 20" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"/></symbol>
<symbol id="list" viewBox="0 0 24 24"><path d="M9 6h11M9 12h11M9 18h11" stroke="currentColor" stroke-width="2" stroke-linecap="round"/><rect x="3.4" y="4.3" width="3.4" height="3.4" rx="1" fill="currentColor"/><rect x="3.4" y="10.3" width="3.4" height="3.4" rx="1" fill="currentColor"/><rect x="3.4" y="16.3" width="3.4" height="3.4" rx="1" fill="currentColor"/></symbol>
<symbol id="grid" viewBox="0 0 24 24"><rect x="4" y="4" width="7" height="7" rx="2" fill="currentColor"/><rect x="13" y="4" width="7" height="7" rx="2" fill="currentColor" opacity=".55"/><rect x="4" y="13" width="7" height="7" rx="2" fill="currentColor" opacity=".55"/><rect x="13" y="13" width="7" height="7" rx="2" fill="currentColor" opacity=".55"/></symbol>
<symbol id="check" viewBox="0 0 24 24"><path d="M4.5 12.8 9.6 17.6 19.5 6.6" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="more" viewBox="0 0 24 24"><circle cx="5.5" cy="12" r="1.8" fill="currentColor"/><circle cx="12" cy="12" r="1.8" fill="currentColor"/><circle cx="18.5" cy="12" r="1.8" fill="currentColor"/></symbol>
<symbol id="copy" viewBox="0 0 24 24"><path d="M15.5 8.5v-3a2 2 0 0 0-2-2h-8a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h3" fill="none" stroke="currentColor" stroke-width="1.8"/><rect x="8.5" y="8.5" width="12" height="12" rx="2.2" fill="none" stroke="currentColor" stroke-width="1.8"/></symbol>
<symbol id="move" viewBox="0 0 24 24"><path d="M3 18V6a1.5 1.5 0 0 1 1.5-1.5H9l2 2.5h8.5A1.5 1.5 0 0 1 21 8.5V18a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18zM8.5 13h7m-2.5-2.6 2.6 2.6-2.6 2.6" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="close" viewBox="0 0 24 24"><path d="M6.5 6.5l11 11m0-11-11 11" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"/></symbol>
<symbol id="play" viewBox="0 0 24 24"><path d="M7 4.6 20 12 7 19.4z" fill="currentColor"/></symbol>
<symbol id="left" viewBox="0 0 24 24"><path d="M15 5.5 8.5 12l6.5 6.5" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="right" viewBox="0 0 24 24"><path d="M9 5.5 15.5 12 9 18.5" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="sliders" viewBox="0 0 24 24"><path d="M4 7h16M4 12h16M4 17h16" stroke="currentColor" stroke-width="2" stroke-linecap="round"/><circle cx="9" cy="7" r="2.4" fill="#1c2032" stroke="currentColor" stroke-width="1.8"/><circle cx="15.5" cy="12" r="2.4" fill="#1c2032" stroke="currentColor" stroke-width="1.8"/><circle cx="7.5" cy="17" r="2.4" fill="#1c2032" stroke="currentColor" stroke-width="1.8"/></symbol>
<symbol id="i-info" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M12 11v5.5" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"/><circle cx="12" cy="7.6" r="1.3" fill="currentColor"/></symbol>
<symbol id="eye" viewBox="0 0 24 24"><path d="M2.5 12C6 5.5 18 5.5 21.5 12 18 18.5 6 18.5 2.5 12z" fill="none" stroke="currentColor" stroke-width="1.8"/><circle cx="12" cy="12" r="3.2" fill="currentColor"/></symbol>
<symbol id="broom" viewBox="0 0 24 24"><path d="M14.5 3.5 10 12m-4.5 1.5h8l1.5 7h-11z M7.5 17.5v3M11 17.5v3" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></symbol>
<symbol id="menu" viewBox="0 0 24 24"><path d="M4 7h16M4 12h16M4 17h16" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></symbol>
<symbol id="ufolder" viewBox="0 0 24 24"><path d="M3 18V6a1.5 1.5 0 0 1 1.5-1.5H9l2 2.5h8.5A1.5 1.5 0 0 1 21 8.5V18a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18zM12 16.5v-6m-2.6 2.6L12 10.5l2.6 2.6" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></symbol>
</defs></svg>

<div class="pin" id="pin" hidden><div class="pinbox"><div class="logo"><svg><use href="#logo"/></svg></div>
<h2>Enter the PIN</h2><p>It's shown on your Switch, in SwitchMX → Wi-Fi.</p>
<div class="digits"><input inputmode="numeric" maxlength="1" autocomplete="off"><input inputmode="numeric" maxlength="1" autocomplete="off"><input inputmode="numeric" maxlength="1" autocomplete="off"><input inputmode="numeric" maxlength="1" autocomplete="off"></div>
<div class="msg" id="pinmsg"></div></div></div>

<div class="app">
<aside id="side"><div class="brand"><div class="logo"><svg><use href="#logo"/></svg></div><div><b>SwitchMX</b><small>Wi-Fi transfer</small></div></div>
<div class="sect">Storage</div><div class="drives" id="drives"></div>
<div class="sidefoot"><span class="dot"></span><span id="conn">Connected to your Switch</span></div>
<div class="srcnote">SwitchMX is free software (AGPL-3.0). <a href="https://github.com/Farzin-Abdi/SwitchMX" target="_blank" rel="noopener">Source code</a></div></aside>
<main>
<div class="top"><button class="iconbtn menu-btn" id="menu"><svg class="i"><use href="#menu"/></svg></button>
<div class="crumbs" id="crumbs"></div>
<div class="search"><svg class="i"><use href="#search"/></svg><input id="q" dir="auto" placeholder="Search this folder and below" autocomplete="off"></div>
<div class="seg"><button class="iconbtn" id="vlist" title="List"><svg class="i"><use href="#list"/></svg></button><button class="iconbtn" id="vgrid" title="Grid"><svg class="i"><use href="#grid"/></svg></button></div></div>
<div class="mcrumbs" id="mcrumbs"></div>
<div class="selbar" id="selbar" hidden><b id="selinfo"></b>
<button class="btn" data-act="download"><svg class="i"><use href="#down"/></svg>Download</button>
<button class="btn" data-act="copy"><svg class="i"><use href="#copy"/></svg>Copy to…</button>
<button class="btn" data-act="move"><svg class="i"><use href="#move"/></svg>Move to…</button>
<button class="btn" data-act="rename" id="selren"><svg class="i"><use href="#edit"/></svg>Rename</button>
<button class="btn danger" data-act="delete"><svg class="i"><use href="#trash"/></svg>Delete</button>
<button class="iconbtn" id="selclear" title="Clear selection"><svg class="i"><use href="#close"/></svg></button></div>
<div class="toolbar"><span class="info" id="info"></span>
<select class="sort" id="sort" title="Sort by"><option value="name">Name</option><option value="mtime">Date modified</option><option value="size">Size</option><option value="type">Type</option></select>
<button class="iconbtn" id="refresh" title="Refresh"><svg class="i"><use href="#refresh"/></svg></button>
<button class="btn" id="viewmenu" title="View options and tools"><svg class="i"><use href="#sliders"/></svg><span>View</span></button>
<button class="btn" id="mkdir"><svg class="i"><use href="#newf"/></svg><span>New folder</span></button>
<button class="btn" id="upfolder"><svg class="i"><use href="#ufolder"/></svg><span>Upload folder</span></button>
<button class="btn primary" id="upload"><svg class="i"><use href="#up"/></svg><span>Upload files</span></button>
<input type="file" id="files" multiple hidden><input type="file" id="dirs" webkitdirectory multiple hidden></div>
<div class="content" id="content"></div>
</main></div>
<div class="drop" id="drop"><div>Drop to upload<small id="dropto"></small></div></div>
<div class="ups" id="ups" hidden><div class="hd"><span class="spin" id="upspin"></span><b id="uptitle">Uploading</b><button class="btn" id="upretry" hidden style="padding:5px 10px">Retry failed</button><button class="iconbtn" id="upclose" style="width:28px;height:28px"><svg class="i"><use href="#close"/></svg></button></div>
<div class="total"><i id="uptotal"></i></div><div class="bd" id="uplist"></div></div>
<div class="toast" id="toast"></div>

<script>
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
const store={get(k,d){try{const v=localStorage.getItem(k);return v===null?d:v}catch(e){return d}},set(k,v){try{localStorage.setItem(k,v)}catch(e){}}};
let showHidden=store.get('smxhidden','0')=='1',foldersFirst=store.get('smxff','1')=='1',thumbs=store.get('smxthumbs','1')=='1',onExists=store.get('smxexists','both');
let PIN=store.get('smxpin',''),roots=[],cwd='',cwdShown='',entries=[],searching=null,sel=new Set(),view=store.get('smxview','list'),sortKey=store.get('smxsort','name'),sortDir=1,lastClick=-1,shown=[];
const K={video:'mp4 mkv avi mov webm m4v ts m2ts wmv flv mpg mpeg 3gp',audio:'mp3 flac m4a aac ogg opus wav wma alac',image:'jpg jpeg png gif webp bmp heic',sub:'srt ass ssa vtt sub',zip:'zip rar 7z tar gz',doc:'pdf epub mobi fb2 xps oxps cbz cbr cb7 cbt docx xlsx pptx'};
const COL={folder:'#5b7cff',video:'#f59e0b',audio:'#10b981',image:'#22b8f0',sub:'#a78bfa',zip:'#ec4899',doc:'#ef5350',file:'#64748b'};
const ICON={folder:'folder',video:'film',audio:'music',image:'image',sub:'sub',zip:'zip',doc:'file',file:'file'};
const ext=n=>(n.includes('.')?n.split('.').pop():'').toLowerCase();
function kind(e){if(e.dir)return'folder';const x=ext(e.name);for(const k in K)if(K[k].split(' ').includes(x))return k;return'file'}
function size(n){if(n<1024)return n+' B';const u=['KB','MB','GB','TB'];let i=-1;do{n/=1024;i++}while(n>=1024&&i<3);return n.toFixed(n<10?1:0)+' '+u[i]}
function dur(s){s=Math.max(0,Math.round(s));if(s<60)return s+' s';const m=Math.floor(s/60);if(m<60)return m+' min'+(m<10&&s%60?' '+s%60+' s':'');const h=Math.floor(m/60);return h+' h'+(m%60?' '+m%60+' min':'')}
function date(t){if(!t)return'';const d=new Date(t*1000);return d.toLocaleDateString(undefined,{day:'2-digit',month:'short',year:'numeric'})+', '+d.toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit'})}
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const q=o=>Object.entries(o).map(([k,v])=>k+'='+encodeURIComponent(v)).join('&');
const fileUrl=(p,dl)=>'/api/file?'+q({path:p,pin:PIN})+(dl?'&dl=1':'');
let toastT;function toast(m,err,busy){const t=$('#toast');t.innerHTML=(busy?'<span class="spin"></span>':'')+esc(m);t.className='toast on'+(err?' err':'');clearTimeout(toastT);if(!busy)toastT=setTimeout(()=>t.classList.remove('on'),3200)}
async function api(path,opt={}){opt.headers=Object.assign({'X-Pin':PIN},opt.headers||{});let r;try{r=await fetch('/api/'+path,opt)}catch(e){conn(false);throw new Error('The Switch is not reachable. Is Wi-Fi transfer still on?')}
conn(true);if(r.status==401){askPin('');throw new Error('PIN')}const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||('Error '+r.status));return j}
function conn(ok){$('#conn').textContent=ok?'Connected to your Switch':'Not connected';$('.dot').style.background=ok?'':'var(--danger)'}
function askPin(msg){$('#pin').hidden=false;$('#pinmsg').textContent=msg;const ins=$$('.digits input');ins.forEach(i=>i.value='');ins[0].focus()}
$$('.digits input').forEach((inp,i,ins)=>{inp.oninput=()=>{inp.value=inp.value.replace(/\D/g,'').slice(-1);if(inp.value&&i<3)ins[i+1].focus();const v=ins.map(x=>x.value).join('');if(v.length==4)tryPin(v)};
inp.onkeydown=e=>{if(e.key=='Backspace'&&!inp.value&&i>0)ins[i-1].focus()};inp.onpaste=e=>{const t=(e.clipboardData.getData('text')||'').replace(/\D/g,'');if(t.length==4){e.preventDefault();tryPin(t)}}});
async function tryPin(v,quiet){PIN=v;try{await api('check');store.set('smxpin',v);$('#pin').hidden=true;start()}catch(e){if(e.message!='PIN')askPin(e.message);else askPin(quiet?'':'That PIN is not right. Check the Switch screen.')}}
async function start(){try{roots=await api('roots')}catch(e){return}drawDrives();if(!cwd||!roots.some(r=>cwd.startsWith(r.path)))cwd=roots.length?roots[0].path:'';if(cwd)open(cwd);else draw()}
function here(){const r=root();return r?(r.name+'/'+(cwdShown||cwd).slice(r.path.length)).replace(/\/$/,''):cwd}
function root(p){p=p===undefined?cwd:p;return roots.find(r=>p.startsWith(r.path))}
function drawDrives(){$('#drives').innerHTML=roots.map((r,i)=>{const used=r.total?(1-r.free/r.total)*100:0,usb=!r.path.startsWith('sdmc');
return `<button class="drive ${root()==r?'on':''}" data-d="${i}"><span class="di" style="color:${usb?'#22d3ee':'#a78bfa'}"><svg class="i"><use href="#${usb?'usb':'sd'}"/></svg></span>
<span style="flex:1;min-width:0"><b>${esc(r.name)}</b><small>${r.total?size(r.free)+' free of '+size(r.total):''}</small><div class="meter"><i class="${used>90?'full':''}" style="width:${used}%"></i></div></span></button>`}).join('')||'<div class="sect" style="text-transform:none">No drives</div>';
$$('.drive').forEach(b=>b.onclick=()=>{open(roots[b.dataset.d].path);$('#side').classList.remove('open')})}
function crumbsHtml(){const r=root();if(!r)return'';const rest=cwd.slice(r.path.length).split('/').filter(Boolean),shown=(cwdShown||cwd).slice(r.path.length).split('/').filter(Boolean);let p=r.path,h=`<button data-p="${esc(p)}">${esc(r.name)}</button>`;
rest.forEach((part,i)=>{p+=part+'/';h+=`<span class="sep">›</span><button data-p="${esc(p)}">${esc(shown[i]||part)}</button>`});return h}
function drawCrumbs(){const h=crumbsHtml();$('#crumbs').innerHTML=h;$('#mcrumbs').innerHTML=h;$$('#crumbs button,#mcrumbs button').forEach(b=>b.onclick=()=>open(b.dataset.p))}
async function open(p,keepSel){try{const j=await api('list?'+q({path:p}));cwd=j.path;cwdShown=j.shown||j.path;entries=j.entries;if(!keepSel){searching=null;$('#q').value='';sel.clear()}drawDrives();drawCrumbs();draw();
try{history.replaceState(null,'','#'+encodeURIComponent(cwd))}catch(e){}}catch(e){if(e.message!='PIN')toast(e.message,true)}}
function up(){const r=root();if(!r||cwd==r.path)return;open(cwd.slice(0,cwd.slice(0,-1).lastIndexOf('/')+1))}
const hidden=e=>e.name.startsWith('.');
function items(){let list=searching?searching.results:entries.map(e=>Object.assign({path:cwd+e.name+(e.dir?'/':'')},e));if(!showHidden)list=list.filter(e=>!hidden(e));
const cmp={name:(a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true,sensitivity:'base'}),mtime:(a,b)=>a.mtime-b.mtime,size:(a,b)=>a.size-b.size,
type:(a,b)=>kind(a).localeCompare(kind(b))||ext(a.name).localeCompare(ext(b.name))||a.name.localeCompare(b.name)};
return list.slice().sort((a,b)=>(foldersFirst?b.dir-a.dir:0)||sortDir*cmp[sortKey](a,b))}
function draw(){shown=items();const n=shown.length,dirs=shown.filter(e=>e.dir).length,bytes=shown.reduce((s,e)=>s+(e.size||0),0);
$('#info').textContent=searching?`${n} result${n==1?'':'s'} for “${searching.q}”${searching.truncated?' (first 400)':''}`:`${dirs} folder${dirs==1?'':'s'}, ${n-dirs} file${n-dirs==1?'':'s'} · ${size(bytes)}`+(showHidden||searching?'':(c=>c?` · ${c} hidden`:'')(entries.filter(hidden).length));
$('#sort').value=sortKey;$('#vlist').classList.toggle('on',view=='list');$('#vgrid').classList.toggle('on',view=='grid');
const c=$('#content');if(!n){c.innerHTML=`<div class="empty"><svg><use href="#${searching?'search':'folder'}"/></svg><b>${searching?'Nothing found':'This folder is empty'}</b>${searching?'Try other words':'Drop files here, or use Upload'}</div>`;drawSel();return}
if(view=='grid'){c.innerHTML='<div class="grid">'+shown.map((e,i)=>{const k=kind(e);return `<div class="tile ${sel.has(e.path)?'sel':''}" data-i="${i}">
<div class="th" style="background:${k=='image'&&thumbs?'#0d0f18':COL[k]+'cc'}">${k=='image'&&thumbs?`<img loading="lazy" src="${fileUrl(e.path)}" alt="">`:`<svg><use href="#${ICON[k]}"/></svg>`}</div>
<div class="cap"><div class="t" dir="auto">${esc(e.name)}</div><small>${e.dir?'Folder':size(e.size)}</small></div>
<span class="chk" data-chk="${i}"><svg><use href="#check"/></svg></span><button class="iconbtn more" data-more="${i}"><svg class="i"><use href="#more"/></svg></button></div>`}).join('')+'</div>'}
else{const ar=k=>sortKey==k?(sortDir>0?' ↑':' ↓'):'';const all=n&&shown.every(e=>sel.has(e.path));
c.innerHTML=`<div class="list"><div class="row head"><span class="chk ${all?'on':''}" id="selall"><svg><use href="#check"/></svg></span><span></span>
<button data-s="name">Name${ar('name')}</button><button class="c-size" data-s="size">Size${ar('size')}</button><button class="c-date" data-s="mtime">Modified${ar('mtime')}</button><span></span></div>`+
shown.map((e,i)=>{const k=kind(e);return `<div class="row ${sel.has(e.path)?'sel':''}" data-i="${i}" style="${hidden(e)?'opacity:.55':''}"><span class="chk" data-chk="${i}"><svg><use href="#check"/></svg></span>
<span class="ico" style="background:${COL[k]}">${k=='image'&&thumbs&&e.size<2e6?`<img loading="lazy" src="${fileUrl(e.path)}" alt="">`:`<svg class="i"><use href="#${ICON[k]}"/></svg>`}</span>
<div class="nm"><div class="t" dir="auto">${esc(e.name)}</div>${searching?`<div class="p" dir="auto">${esc(where(e))}</div>`:''}</div>
<span class="meta c-size">${e.dir?'—':size(e.size)}</span><span class="meta c-date">${date(e.mtime)}</span>
<button class="iconbtn" data-more="${i}"><svg class="i"><use href="#more"/></svg></button></div>`}).join('')+'</div>';
$$('[data-s]').forEach(b=>b.onclick=()=>{if(sortKey==b.dataset.s)sortDir=-sortDir;else{sortKey=b.dataset.s;sortDir=1;store.set('smxsort',sortKey)}draw()});
$('#selall').onclick=()=>{if(all)shown.forEach(e=>sel.delete(e.path));else shown.forEach(e=>sel.add(e.path));draw()}}
$$('[data-i]').forEach(el=>{const i=+el.dataset.i,e=shown[i];
el.onclick=ev=>{if(ev.target.closest('[data-more]'))return;if(ev.target.closest('[data-chk]')||ev.metaKey||ev.ctrlKey){toggle(e.path);lastClick=i;return}
if(ev.shiftKey&&lastClick>=0){const a=Math.min(lastClick,i),b=Math.max(lastClick,i);for(let k=a;k<=b;k++)sel.add(shown[k].path);draw();return}
if(sel.size){toggle(e.path);lastClick=i;return}activate(e)};
el.oncontextmenu=ev=>{ev.preventDefault();if(!sel.has(e.path)){sel.clear();sel.add(e.path);draw()}menu(ev.clientX,ev.clientY,e)}});
$$('[data-more]').forEach(b=>b.onclick=ev=>{ev.stopPropagation();const r=b.getBoundingClientRect();menu(r.left-170,r.bottom+4,shown[+b.dataset.more])});drawSel()}
function where(e){if(e.where!==undefined){const r=root(e.path);return r?(r.name+'/'+e.where.slice(r.path.length)).replace(/\/$/,''):e.where}const dir=e.path.slice(0,e.path.length-e.name.length-(e.dir?1:0)),r=root(dir);return r?(r.name+'/'+dir.slice(r.path.length)).replace(/\/$/,''):dir}
function toggle(p){sel.has(p)?sel.delete(p):sel.add(p);draw()}
function drawSel(){$('#selbar').hidden=!sel.size;$('#selinfo').textContent=sel.size+' selected';$('#selren').hidden=sel.size!=1}
function activate(e){if(e.dir){open(e.path);return}const k=kind(e);if(['image','video','audio','sub'].includes(k)||['txt','log','ini','json','nfo','md','cfg'].includes(ext(e.name)))preview(e);else if(ext(e.name)=='pdf')window.open(fileUrl(e.path),'_blank');else location.href=fileUrl(e.path,1)}
function picked(){return shown.filter(e=>sel.has(e.path))}
function menu(x,y,e){closeMenu();const m=document.createElement('div');m.className='ctx';m.id='ctx';const many=sel.size>1&&sel.has(e.path);
const its=[!many&&['Open','play',()=>activate(e)],!many&&['Info','i-info',()=>info(e)],[many?'Download '+sel.size+' as ZIP':e.dir?'Download as ZIP':'Download','down',()=>download(many?picked():[e])],
'-',!many&&['Rename','edit',()=>rename(e)],['Copy to…','copy',()=>transfer('copy',many?picked():[e])],['Move to…','move',()=>transfer('move',many?picked():[e])],'-',
[many?'Delete '+sel.size+' items':'Delete','trash',()=>del(many?picked():[e]),'danger']].filter(Boolean);
m.innerHTML=its.map((it,i)=>it=='-'?'<hr>':`<button data-m="${i}" class="${it[3]||''}"><svg class="i"><use href="#${it[1]}"/></svg>${esc(it[0])}</button>`).join('');
document.body.appendChild(m);const r=m.getBoundingClientRect();m.style.left=Math.max(8,Math.min(x,innerWidth-r.width-8))+'px';m.style.top=Math.max(8,Math.min(y,innerHeight-r.height-8))+'px';
m.querySelectorAll('[data-m]').forEach(b=>b.onclick=()=>{closeMenu();its[+b.dataset.m][2]()})}
function viewMenu(btn){closeMenu();const m=document.createElement('div');m.className='ctx';m.id='ctx';
const tick=on=>`<span class="tick ${on?'on':''}"><svg><use href="#check"/></svg></span>`;
m.innerHTML=`<div class="lbl">Show</div><button data-v="hidden"><svg class="i"><use href="#eye"/></svg>Hidden files${tick(showHidden)}</button>
<button data-v="ff"><svg class="i"><use href="#folder"/></svg>Folders first${tick(foldersFirst)}</button>
<button data-v="thumbs"><svg class="i"><use href="#image"/></svg>Photo thumbnails${tick(thumbs)}</button><hr>
<div class="lbl">When an upload is already there</div>${[['both','Keep both'],['replace','Replace it'],['skip','Skip it']].map(([k,t])=>`<button data-x="${k}"><svg class="i"><use href="#${k=='both'?'copy':k=='replace'?'up':'close'}"/></svg>${t}${tick(onExists==k)}</button>`).join('')}<hr>
<div class="lbl">Tools for this folder</div><button data-v="du"><svg class="i"><use href="#i-info"/></svg>Folder size</button>
<button data-v="clean"><svg class="i"><use href="#broom"/></svg>Remove Mac junk files…</button>`;
document.body.appendChild(m);const r=btn.getBoundingClientRect(),mr=m.getBoundingClientRect();m.style.left=Math.max(8,Math.min(r.right-mr.width,innerWidth-mr.width-8))+'px';m.style.top=(r.bottom+6)+'px';
m.querySelectorAll('[data-x]').forEach(b=>b.onclick=ev=>{ev.stopPropagation();onExists=b.dataset.x;store.set('smxexists',onExists);viewMenu(btn)});
m.querySelectorAll('[data-v]').forEach(b=>b.onclick=ev=>{ev.stopPropagation();const v=b.dataset.v;
if(v=='hidden'){showHidden=!showHidden;store.set('smxhidden',showHidden?'1':'0');toast(showHidden?'Showing hidden files':'Hiding hidden files')}
if(v=='ff'){foldersFirst=!foldersFirst;store.set('smxff',foldersFirst?'1':'0')}
if(v=='thumbs'){thumbs=!thumbs;store.set('smxthumbs',thumbs?'1':'0')}
if(v=='du'){closeMenu();info({name:here().split('/').pop(),path:cwd,dir:true});return}
if(v=='clean'){closeMenu();cleanup();return}
draw();viewMenu(btn)})}
async function info(e){const m=modal(`<div class="dlg"><h3 style="word-break:break-all" dir="auto">${esc(e.name)}</h3><div class="kv">
<span>Type</span><span>${e.dir?'Folder':(ext(e.name)||'file').toUpperCase()+' file'}</span><span>Location</span><span>${esc(where(e))}</span>
<span>Size</span><span id="kvsize">${e.dir?'<span class="spin"></span> counting…':size(e.size)+' ('+e.size.toLocaleString()+' bytes)'}</span>
${e.mtime?`<span>Modified</span><span>${date(e.mtime)}</span>`:''}</div><div class="acts"><button class="btn primary" data-c>Close</button></div></div>`);
const done=()=>m.remove();m.querySelector('[data-c]').onclick=done;m.onclick=x=>{if(x.target==m)done()};
if(e.dir){try{const j=await api('du?'+q({path:e.path}));const el=m.querySelector('#kvsize');if(el)el.textContent=`${size(j.bytes)} · ${j.files} file${j.files==1?'':'s'}, ${j.folders} folder${j.folders==1?'':'s'}${j.partial?' (still counting when it stopped)':''}`}
catch(x){const el=m.querySelector('#kvsize');if(el)el.textContent=x.message}}}
async function cleanup(){if(!await confirm2('Remove Mac junk files?','Deletes the "._" files and .DS_Store that a Mac leaves behind, in this folder and every folder inside it. Your own files are not touched.','Remove'))return;
toast('Cleaning up…',false,true);try{const j=await api('cleanup?'+q({path:cwd}),{method:'POST'});toast(j.removed?`Removed ${j.removed} junk file${j.removed==1?'':'s'} (${size(j.bytes)})`:'Nothing to clean up here');refresh()}catch(x){toast(x.message,true)}}
function closeMenu(){const m=$('#ctx');if(m)m.remove()}addEventListener('click',e=>{if(!e.target.closest('.ctx'))closeMenu()},true);
function download(list){if(!list.length)return;if(list.length==1&&!list[0].dir){location.href=fileUrl(list[0].path,1);return}
const r=root(),name=(list.length==1?list[0].name:(r?r.name:'SwitchMX')+' files')+'.zip';
const a=document.createElement('a');a.href='/api/zip?'+q({paths:list.map(e=>e.path).join('\n'),name,pin:PIN});a.download=name;document.body.appendChild(a);a.click();a.remove();toast('Downloading '+name)}
async function rename(e){const v=await ask('Rename','',e.name,'Rename');if(!v||v==e.name)return;try{await api('rename?'+q({path:e.path,name:v}),{method:'POST'});sel.clear();toast('Renamed to '+v);refresh()}catch(x){toast(x.message,true)}}
async function del(list){if(!list.length)return;const what=list.length==1?'“'+list[0].name+'”':list.length+' items';
if(!await confirm2('Delete '+what+'?','This permanently removes '+(list.length==1&&list[0].dir?'the folder and everything in it':list.length==1?'it':'them')+' from the Switch. It can\'t be undone.','Delete'))return;
toast('Deleting…',false,true);let n=0;for(const e of list){try{await api('delete?'+q({path:e.path}),{method:'POST'});n++}catch(x){toast(x.message,true);break}}sel.clear();if(n)toast('Deleted '+(n==1?list[0].name:n+' items'));refresh()}
async function transfer(how,list){if(!list.length)return;const dest=await pickFolder((how=='move'?'Move ':'Copy ')+(list.length==1?'“'+list[0].name+'”':list.length+' items'),how=='move'?'Move here':'Copy here',list.length>1);
if(!dest)return;toast((how=='move'?'Moving':'Copying')+'… large files take a while',false,true);
try{const j=await api(how+'?'+q({path:dest}),{method:'POST',body:list.map(e=>e.path).join('\n')});sel.clear();toast((how=='move'?'Moved ':'Copied ')+(j.done==1?list[0].name:j.done+' items'));refresh()}catch(x){toast(x.message,true);refresh()}}
async function mkdir(){const r=root();const v=await ask('New folder','In '+here(),'','Create');if(!v)return;
try{await api('mkdir?'+q({path:cwd,name:v}),{method:'POST'});await open(cwd);toast('Folder created')}catch(x){toast(x.message,true)}}
function refresh(){searching?runSearch(searching.q):open(cwd,true)}
function modal(html){const m=document.createElement('div');m.className='modal';m.innerHTML=html;document.body.appendChild(m);return m}
function ask(title,text,val,ok){return new Promise(res=>{const m=modal(`<div class="dlg"><h3>${esc(title)}</h3>${text?`<p>${esc(text)}</p>`:''}<input value="${esc(val)}" dir="auto"><div class="acts"><button class="btn" data-c>Cancel</button><button class="btn primary" data-o>${esc(ok)}</button></div></div>`);
const i=m.querySelector('input'),done=v=>{m.remove();res(v)};i.focus();const dot=val.lastIndexOf('.');i.setSelectionRange(0,dot>0?dot:val.length);
m.querySelector('[data-o]').onclick=()=>done(i.value.trim());m.querySelector('[data-c]').onclick=()=>done(null);i.onkeydown=e=>{if(e.key=='Enter')done(i.value.trim());if(e.key=='Escape')done(null)};m.onclick=e=>{if(e.target==m)done(null)}})}
function confirm2(title,text,ok){return new Promise(res=>{const m=modal(`<div class="dlg"><h3>${esc(title)}</h3><p>${esc(text)}</p><div class="acts"><button class="btn" data-c>Cancel</button><button class="btn danger" data-o>${esc(ok)}</button></div></div>`);
const k=e=>{if(e.key=='Escape')done(false)},done=v=>{m.remove();removeEventListener('keydown',k);res(v)};addEventListener('keydown',k);m.querySelector('[data-o]').onclick=()=>done(true);m.querySelector('[data-c]').onclick=()=>done(false);m.querySelector('[data-o]').focus()})}
function pickFolder(title,ok,many){return new Promise(res=>{let at=cwd;const m=modal(`<div class="dlg picker"><div class="ph"><h3>${esc(title)}</h3><p style="margin:0">Choose where to put ${many?'them':'it'}. You can pick another drive.</p></div><div class="pc"></div><div class="pl"></div>
<div class="pf"><button class="btn" data-n style="margin-right:auto"><svg class="i"><use href="#newf"/></svg>New folder</button><button class="btn" data-c>Cancel</button><button class="btn primary" data-o>${esc(ok)}</button></div></div>`);
const done=v=>{m.remove();res(v)};
async function go(p){at=p;const r=root(p);let dirs=[],lbl=p;try{const j=await api('list?'+q({path:p}));lbl=j.shown||p;dirs=j.entries.filter(e=>e.dir).sort((a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true}))}catch(e){}if(at!=p)return;
let html='';if(r){const rest=p.slice(r.path.length).split('/').filter(Boolean),nm=lbl.slice(r.path.length).split('/').filter(Boolean);let x=r.path;html=`<button data-p="${esc(x)}">${esc(r.name)}</button>`;rest.forEach((s,i)=>{x+=s+'/';html+=` › <button data-p="${esc(x)}" dir="auto">${esc(nm[i]||s)}</button>`})}
m.querySelector('.pc').innerHTML=roots.map(r2=>`<button data-p="${esc(r2.path)}" style="${r2==r?'color:var(--text);font-weight:650;background:#7c6bff22':''}"><svg class="i" style="vertical-align:-4px;margin-right:4px"><use href="#${r2.path.startsWith('sdmc')?'sd':'usb'}"/></svg>${esc(r2.name)}</button>`).join(' ')+'<br>'+html;
m.querySelector('.pl').innerHTML=dirs.map(d=>`<button data-p="${esc(d.path||p+d.name+'/')}"><span class="ico" style="background:${COL.folder};width:30px;height:30px"><svg class="i"><use href="#folder"/></svg></span><span dir="auto">${esc(d.name)}</span></button>`).join('')||'<div class="empty" style="padding:40px">No folders here</div>';
m.querySelectorAll('[data-p]').forEach(b=>b.onclick=()=>go(b.dataset.p))}
m.querySelector('[data-o]').onclick=()=>done(at);m.querySelector('[data-c]').onclick=()=>done(null);m.onclick=e=>{if(e.target==m)done(null)};
m.querySelector('[data-n]').onclick=async()=>{const v=await ask('New folder','','','Create');if(!v)return;try{const j=await api('mkdir?'+q({path:at,name:v}),{method:'POST'});go(j.path||at)}catch(x){toast(x.message,true)}};go(at)})}
function preview(e){const media=shown.filter(x=>!x.dir&&['image','video','audio'].includes(kind(x)));let idx=media.indexOf(e);
const v=document.createElement('div');v.className='viewer';document.body.appendChild(v);
function show(item){const k=kind(item),u=fileUrl(item.path);let body;
if(k=='image')body=`<img src="${u}" alt="">`;else if(k=='video')body=`<video src="${u}" controls autoplay playsinline></video>`;
else if(k=='audio')body=`<div style="text-align:center"><div class="ico" style="width:120px;height:120px;border-radius:28px;margin:0 auto 22px;background:${COL.audio}"><svg style="width:60px;height:60px"><use href="#music"/></svg></div><audio src="${u}" controls autoplay></audio></div>`;
else body='<pre>Loading…</pre>';
v.innerHTML=`<div class="vh"><b dir="auto">${esc(item.name)}</b><span style="color:#fff9;font-size:13px">${size(item.size)}</span><a class="btn" href="${fileUrl(item.path,1)}"><svg class="i"><use href="#down"/></svg>Download</a><button class="iconbtn" data-x style="color:#fff"><svg class="i"><use href="#close"/></svg></button></div>
<div class="vb">${body}${idx>=0&&media.length>1?'<button class="nav l"><svg class="i"><use href="#left"/></svg></button><button class="nav r"><svg class="i"><use href="#right"/></svg></button>':''}</div>`;
if(!['image','video','audio'].includes(k))fetch(u,{headers:{Range:'bytes=0-524287'}}).then(r=>r.text()).then(t=>{const p=v.querySelector('pre');if(p)p.textContent=t}).catch(()=>{});
if(k=='video')v.querySelector('video').onerror=()=>toast('Your browser can\'t play this kind of video. Download it, or play it on the Switch.',true);
v.querySelector('[data-x]').onclick=close;const l=v.querySelector('.nav.l'),r=v.querySelector('.nav.r');if(l){l.onclick=()=>step(-1);r.onclick=()=>step(1)}}
function step(d){if(idx<0)return;idx=(idx+d+media.length)%media.length;show(media[idx])}
function close(){v.remove();removeEventListener('keydown',key)}
function key(ev){if(ev.key=='Escape')close();if(ev.key=='ArrowLeft')step(-1);if(ev.key=='ArrowRight')step(1)}
addEventListener('keydown',key);show(e)}
let sT;$('#q').oninput=()=>{clearTimeout(sT);const v=$('#q').value.trim();if(!v){if(searching){searching=null;draw()}return}sT=setTimeout(()=>runSearch(v),350)};
async function runSearch(v){$('#info').innerHTML='<span class="spin" style="vertical-align:-3px;margin-right:8px"></span>Searching…';
try{const j=await api('search?'+q({path:cwd,q:v}));if($('#q').value.trim()!=v)return;searching={q:v,results:j.results,truncated:j.truncated};sel.clear();draw()}catch(x){toast(x.message,true)}}
const queue=[];let active=[],doneBytes=0,totalBytes=0,failed=0,okCount=0,skipped=0,failedJobs=[];const PARALLEL=2;
function enqueue(list){if(!list.length)return;$('#ups').hidden=false;for(const it of list){const file=it.file,rel=(it.rel||file.name).normalize('NFC'),el=document.createElement('div');el.className='up';
el.innerHTML=`<span class="n" dir="auto">${esc(rel)}</span><button class="iconbtn" style="width:26px;height:26px" title="Cancel"><svg class="i"><use href="#close"/></svg></button><span class="s">${size(file.size)} · waiting</span><div class="bar"><i></i></div>`;
$('#uplist').appendChild(el);const job={file,rel,el,dir:it.dir||cwd,cancel:false,mode:onExists};el.querySelector('button').onclick=()=>{job.cancel=true;if(job.xhr)job.xhr.abort();else{el.classList.add('err');el.querySelector('.s').textContent='Cancelled';el.querySelector('button').remove();totalBytes-=file.size;paint()}};
queue.push(job);totalBytes+=file.size}paint();next()}
function paint(){const left=queue.filter(j=>!j.cancel).length+active.length;$('#uptitle').textContent=left?`Uploading ${left} file${left==1?'':'s'}`:(failed?`${okCount} uploaded, ${failed} not uploaded`:`${okCount} uploaded`)+(skipped?`, ${skipped} already there`:'');$('#upretry').hidden=!!left||!failedJobs.length;
$('#upspin').hidden=!left;const cur=active.reduce((a,j)=>a+(j.sent||0),0);$('#uptotal').style.width=(totalBytes?Math.min(100,(doneBytes+cur)/totalBytes*100):100)+'%'}
function next(){while(active.length<PARALLEL&&queue.length){const job=queue.shift();if(!job.cancel)startUpload(job)}paint()}
async function startUpload(job){active.push(job);
if(job.mode=='skip'){try{const j=await api('exists?'+q({path:job.dir,name:job.rel}));if(j.exists){active=active.filter(a=>a!=job);skipped++;totalBytes-=job.file.size;job.el.classList.add('done');job.el.querySelector('.s').textContent='Already there, skipped';const b=job.el.querySelector('button');if(b)b.remove();paint();next();return}}catch(e){}if(job.cancel){active=active.filter(a=>a!=job);paint();next();return}}
const x=new XMLHttpRequest(),t0=Date.now(),s=job.el.querySelector('.s'),bar=job.el.querySelector('i');job.xhr=x;
x.open('PUT','/api/upload?'+q({path:job.dir,name:job.rel,exists:job.mode=='replace'?'replace':'both'}));x.setRequestHeader('X-Pin',PIN);
x.upload.onprogress=e=>{if(!e.lengthComputable)return;job.sent=e.loaded;const sec=Math.max(.2,(Date.now()-t0)/1000),sp=e.loaded/sec;bar.style.width=(e.loaded/e.total*100)+'%';
s.textContent=`${size(e.loaded)} of ${size(e.total)} · ${size(sp)}/s · ${dur((e.total-e.loaded)/sp)} left`;paint()};
const fin=(ok,msg)=>{active=active.filter(j=>j!=job);doneBytes+=job.file.size;job.sent=0;job.el.classList.add(ok?'done':'err');if(ok){okCount++;bar.style.width='100%';let j2={};try{j2=JSON.parse(x.responseText)}catch(e){}
const base=job.rel.split('/').pop();s.textContent=size(job.file.size)+(j2.name&&j2.name!=base?' · saved as “'+j2.name+'” (that name was taken)':' · done')}else{failed++;if(msg!='Cancelled')failedJobs.push(job);s.textContent=msg}
const b=job.el.querySelector('button');if(b)b.remove();if(job.dir==cwd&&!searching&&!queue.some(j=>j.dir==cwd&&!j.cancel)&&!active.some(j=>j.dir==cwd))open(cwd,true);paint();next()};
x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(e){}x.status==200?fin(true):fin(false,j.error||('Failed ('+x.status+')'))};
x.onerror=()=>fin(false,'Connection lost');x.onabort=()=>fin(false,'Cancelled');x.send(job.file)}
$('#upretry').onclick=()=>{const l=failedJobs.splice(0);failed-=l.length;l.forEach(j=>{doneBytes-=j.file.size;totalBytes-=j.file.size;j.el.remove()});enqueue(l.map(j=>({file:j.file,rel:j.rel,dir:j.dir})))};
$('#upclose').onclick=()=>{if(active.length||queue.length){if(!window.confirm('Stop the uploads that are still running?'))return;queue.length=0;active.slice().forEach(j=>j.xhr&&j.xhr.abort())}$('#ups').hidden=true;$('#uplist').innerHTML='';doneBytes=totalBytes=failed=okCount=skipped=0;failedJobs=[]};
$('#upload').onclick=()=>$('#files').click();$('#upfolder').onclick=()=>$('#dirs').click();
$('#files').onchange=e=>{enqueue([...e.target.files].map(f=>({file:f})));e.target.value=''};
$('#dirs').onchange=e=>{enqueue([...e.target.files].map(f=>({file:f,rel:f.webkitRelativePath||f.name})));e.target.value=''};
async function walk(entry,prefix,out){if(entry.isFile)await new Promise(r=>entry.file(f=>{out.push({file:f,rel:prefix+f.name});r()},r));
else if(entry.isDirectory){const rd=entry.createReader();let batch;do{batch=await new Promise(r=>rd.readEntries(r,()=>r([])));for(const e of batch)await walk(e,prefix+entry.name+'/',out)}while(batch.length)}}
let dragN=0;addEventListener('dragenter',e=>{if(!e.dataTransfer||![...e.dataTransfer.types].includes('Files'))return;e.preventDefault();dragN++;const r=root();$('#dropto').textContent='into '+here();$('#drop').classList.add('on')});
addEventListener('dragleave',()=>{if(--dragN<=0){dragN=0;$('#drop').classList.remove('on')}});addEventListener('dragover',e=>e.preventDefault());
addEventListener('drop',async e=>{e.preventDefault();dragN=0;$('#drop').classList.remove('on');const its=[...(e.dataTransfer.items||[])].map(i=>i.webkitGetAsEntry&&i.webkitGetAsEntry()).filter(Boolean);
if(its.length){const out=[];for(const it of its)await walk(it,'',out);enqueue(out)}else enqueue([...e.dataTransfer.files].map(f=>({file:f})))});
addEventListener('beforeunload',e=>{if(active.length||queue.length){e.preventDefault();e.returnValue=''}});
$('#mkdir').onclick=mkdir;$('#viewmenu').onclick=ev=>{ev.stopPropagation();if($('#ctx'))closeMenu();else viewMenu(ev.currentTarget)};$('#refresh').onclick=refresh;$('#sort').onchange=e=>{sortKey=e.target.value;sortDir=1;store.set('smxsort',sortKey);draw()};
$('#vlist').onclick=()=>{view='list';store.set('smxview',view);draw()};$('#vgrid').onclick=()=>{view='grid';store.set('smxview',view);draw()};
$('#menu').onclick=()=>$('#side').classList.toggle('open');$('#selclear').onclick=()=>{sel.clear();draw()};
$$('#selbar [data-act]').forEach(b=>b.onclick=()=>{const l=picked(),a=b.dataset.act;if(a=='download')download(l);if(a=='copy'||a=='move')transfer(a,l);if(a=='delete')del(l);if(a=='rename'&&l.length==1)rename(l[0])});
addEventListener('keydown',e=>{if(e.target.tagName=='INPUT'||$('.modal')||$('.viewer'))return;
if((e.ctrlKey||e.metaKey)&&e.key=='a'){e.preventDefault();shown.forEach(x=>sel.add(x.path));draw()}
else if(e.key=='Escape'){if(sel.size){sel.clear();draw()}else if(searching){$('#q').value='';searching=null;draw()}}
else if(e.key=='Delete'&&sel.size)del(picked());else if(e.key=='F2'&&sel.size==1)rename(picked()[0]);else if(e.key=='Backspace'){e.preventDefault();up()}
else if(e.key=='/'){e.preventDefault();$('#q').focus()}});
setInterval(async()=>{if(document.hidden||!PIN||!$('#pin').hidden)return;try{const r=await api('roots');const was=roots.map(x=>x.path).join();roots=r;drawDrives();
if(was!=roots.map(x=>x.path).join()&&!root()&&roots.length)open(roots[0].path)}catch(e){}},5000);
cwd=decodeURIComponent(location.hash.slice(1)||'');const qp=new URLSearchParams(location.search).get('pin');if(qp){PIN=qp;try{history.replaceState(null,'',location.pathname+location.hash)}catch(e){}}
if(PIN)tryPin(PIN,true);else askPin('');
</script></body></html>)HTML";
