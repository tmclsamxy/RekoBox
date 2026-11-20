// 重新生成效果图：node shoot.js（需要 playwright-core 与本机 Chrome）
// 输出：同目录 mock-*.png
const { chromium } = require("playwright-core");
const path = require("path");
const fs = require("fs");

const DIR = __dirname;
const FILE = path.join(DIR, "index.html");
const NAMES = ["A1-light", "A2-dark", "B1-sidebar", "B2-popover", "C1-prefs"];
// 本机 Chrome：Windows / macOS 各一个默认值，可用 CHROME_PATH 覆盖
const CHROME = process.env.CHROME_PATH || (process.platform === "darwin"
  ? "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
  : "C:/Program Files/Google/Chrome/Application/chrome.exe");

(async () => {
  const browser = await chromium.launch({
    executablePath: CHROME,
    args: ["--no-proxy-server", "--allow-file-access-from-files"],
    headless: true,
  });
  const ctx = await browser.newContext({ viewport: { width: 1100, height: 1000 }, deviceScaleFactor: 2 });
  const page = await ctx.newPage();
  const errors = [];
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push("pageerror: " + e.message));

  await page.goto("file:///" + FILE.replace(/\\/g, "/"), { waitUntil: "load" });
  await page.waitForTimeout(600);

  const figs = await page.$$("figure");
  for (let i = 0; i < figs.length; i++) {
    await figs[i].screenshot({ path: path.join(DIR, `mock-${NAMES[i] || i}.png`) });
  }
  console.log("figures:", figs.length, "errors:", JSON.stringify(errors));
  await browser.close();
})();
