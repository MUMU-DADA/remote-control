#!/usr/bin/env python3
"""Real-browser regression checks for streaming, backpressure and reconnects."""

import argparse
import os
import re
from pathlib import Path

from playwright.sync_api import sync_playwright


CHROME = os.environ.get(
    "CHROME", "/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome"
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:18089")
    parser.add_argument("--shots", default="/tmp/rc-webui-stream")
    parser.add_argument("--html", help="Test a generated HTML file against the live API")
    parser.add_argument("--h264", action="store_true", help="Also verify device WebCodecs streaming")
    args = parser.parse_args()
    html = Path(args.html).read_text(encoding="utf-8") if args.html else None
    checks = []
    errors = []

    def check(label, condition):
        checks.append(bool(condition))
        print(f"{'PASS' if condition else 'FAIL'} {label}")

    def pixels(page):
        return page.evaluate("""() => {
          const pixels = ctx.getImageData(0, 0, cvs.width, cvs.height).data;
          const colors = new Set(); let nonblack = 0;
          for (let i = 0; i < pixels.length; i += 4 * 97) {
            const color = pixels[i] * 65536 + pixels[i + 1] * 256 + pixels[i + 2];
            colors.add(color); if (color) ++nonblack;
          }
          return {width: cvs.width, height: cvs.height, colors: colors.size,
                  nonblack, shownFps, format: streamFormat};
        }""")

    with sync_playwright() as playwright:
        browser_args = ["--no-sandbox", "--disable-gpu"]
        if html is not None:
            # A fulfilled page has no server IP; permit its live loopback API.
            browser_args.append("--disable-features=LocalNetworkAccessChecks")
        browser = playwright.chromium.launch(
            headless=True, executable_path=CHROME,
            args=browser_args,
        )
        for name, viewport in (
            ("desktop", {"width": 1366, "height": 900}),
            ("mobile", {"width": 390, "height": 844}),
        ):
            page = browser.new_page(viewport=viewport)
            page.on("pageerror", lambda error: errors.append(str(error)))
            if html is not None:
                page.route(re.compile(re.escape(args.base) + r"/(?:\?.*)?$"),
                           lambda route: route.fulfill(content_type="text/html", body=html))
            page.goto(args.base + "/?format=png&fps=30&quality=1",
                      wait_until="domcontentloaded")
            page.wait_for_function("streamReady && shownFps > 0")
            initial = pixels(page)
            check(name + " canvas contains a real image",
                  initial["width"] > 0 and initial["nonblack"] > 0
                  and initial["colors"] > 1)
            shot = Path(args.shots + "-" + name + ".png").resolve()
            page.screenshot(path=str(shot))
            print("Screenshot:", shot)
            if name == "mobile":
                rect = page.locator("#screen").bounding_box()
                check("mobile keeps the image large enough to inspect",
                      rect["width"] >= 200 and rect["height"] >= 200)
                page.evaluate("togglePanel()")
                canvas = page.locator("#screen").bounding_box()
                panel = page.locator(".panel").bounding_box()
                check("mobile opens controls below the image without overlap",
                      panel and canvas["width"] >= 180
                      and canvas["y"] + canvas["height"] <= panel["y"])
                panel_shot = Path(args.shots + "-mobile-panel.png").resolve()
                page.screenshot(path=str(panel_shot))
                print("Screenshot:", panel_shot)
                page.evaluate("togglePanel()")

            for fmt, quality in (("webp", 80), ("png", 1), ("webp", 42)):
                page.evaluate("([fmt, q]) => { setCodec(fmt, q); "
                              "skipUnchanged = false; "
                              "sendStream({t:'skipUnchanged', v:0}); }",
                              [fmt, quality])
                page.wait_for_function("fmt => streamFormat === fmt && shownFps > 0",
                                       arg=fmt)
                page.wait_for_timeout(250)
                state = pixels(page)
                check(name + " switches to " + fmt + " without blanking",
                      state["format"] == fmt and state["nonblack"] > 0)

            replaced = page.evaluate("""() => {
              const previous = streamWs;
              document.getElementById('tokeninput').value = '';
              saveToken();
              return streamWs && streamWs !== previous;
            }""")
            check(name + " saving the token immediately creates a new stream",
                  replaced)
            page.wait_for_function("streamReady && shownFps > 0")

            if name == "desktop":
                # Disconnect the producer while retaining the original handler
                # closure, then feed valid PNG frames through that same handler.
                page.evaluate("""() => {
                  window.testSocket = streamWs;
                  window.testMessage = streamWs.onmessage;
                  window.testClose = streamWs.onclose;
                  window.testError = streamWs.onerror;
                  streamWs.onmessage = null; streamWs.onclose = null;
                  streamWs.close(); streamReady = false;
                  stopH264(); resetPresentation(); streamFormat = 'png';
                  window.bitmapStats = {active:0, maxActive:0, calls:0};
                  window.realCreateImageBitmap = window.createImageBitmap;
                  window.createImageBitmap = async blob => {
                    ++bitmapStats.calls; ++bitmapStats.active;
                    bitmapStats.maxActive = Math.max(bitmapStats.maxActive,
                                                      bitmapStats.active);
                    try {
                      await new Promise(resolve => setTimeout(resolve, 120));
                      return await realCreateImageBitmap(blob);
                    } finally { --bitmapStats.active; }
                  };
                  window.testPng = async color => {
                    const canvas = document.createElement('canvas');
                    canvas.width = 64; canvas.height = 96;
                    const context = canvas.getContext('2d');
                    context.fillStyle = color; context.fillRect(0,0,64,96);
                    const blob = await new Promise(resolve => canvas.toBlob(resolve));
                    return blob.arrayBuffer();
                  };
                }""")
                result = page.evaluate("""async () => {
                  const frames = await Promise.all(['red','green','blue'].map(testPng));
                  for (const data of frames) await testMessage({data});
                  await new Promise(resolve => setTimeout(resolve, 400));
                  const pixel = Array.from(ctx.getImageData(1,1,1,1).data);
                  return {...bitmapStats, pixel};
                }""")
                check("slow decoder has only one in-flight decode",
                      result["maxActive"] == 1)
                check("slow decoder skips the overwritten middle frame",
                      result["calls"] == 2 and result["pixel"][2] > 240
                      and result["pixel"][0] < 10)

                result = page.evaluate("""async () => {
                  const data = await testPng('red');
                  await testMessage({data});
                  stopStream(); startStream();
                  const current = streamWs;
                  testClose({code:1000, wasClean:true, reason:'old connection'});
                  testError({message:'old connection'});
                  await testMessage({data});
                  return {same: streamWs === current, ready: streamReady,
                          calls: bitmapStats.calls};
                }""")
                check("old socket events cannot close or clear the new connection",
                      result["same"] and result["calls"] == 3)
                page.wait_for_function("streamReady && cvs.width > 64")
                page.wait_for_timeout(300)
                check("reconnect replaces stale decoded pixels with live image",
                      pixels(page)["colors"] > 1)
                page.evaluate("() => { window.createImageBitmap = realCreateImageBitmap; }")
                state = page.evaluate("""async () => {
                  const socket = streamWs, format = streamFormat;
                  codec = 'h264'; quality = 75;
                  await socket.onmessage({data:JSON.stringify(
                    {t:'error', error:'unsupported format'})});
                  return {same: socket === streamWs, codec, format,
                          error: $('status').classList.contains('err')};
                }""")
                check("unsupported format restores the actual format and keeps the stream",
                      state["same"] and state["codec"] == state["format"]
                      and state["error"])
                state = page.evaluate("""async () => {
                  const socket = streamWs, oldClose = socket.onclose;
                  codec = 'h264'; streamFormat = 'h264';
                  await socket.onmessage({data:JSON.stringify(
                    {t:'error', error:'encoder unavailable'})});
                  const fallback = codec === 'jpeg' && streamWs === null
                         && $('status').textContent.includes('encoder unavailable');
                  // The stub has no JPEG. Restore PNG immediately after proving
                  // the fallback, so reconnection can exercise a real stream.
                  codec = 'png'; quality = 1; startStream();
                  const current = streamWs;
                  oldClose({code:1000, wasClean:true, reason:'encoder failed'});
                  return {fallback, same: current === streamWs};
                }""")
                check("fatal H.264 errors fall back and stale close preserves the replacement",
                      state["fallback"] and state["same"])
                page.wait_for_function("streamReady && streamFormat === 'png'")

            page.evaluate("fps = 0; stopStream()")
            page.close()
        if args.h264:
            page = browser.new_page(viewport={"width": 1366, "height": 900})
            page.on("pageerror", lambda error: errors.append(str(error)))
            frames = []

            def observe_socket(socket):
                if "/api/v1/stream" in socket.url:
                    socket.on("framereceived", lambda data: frames.append(data)
                              if isinstance(data, bytes) else None)

            page.on("websocket", observe_socket)
            page.add_init_script("""(() => {
              window.renderCount = 0;
              const draw = CanvasRenderingContext2D.prototype.drawImage;
              CanvasRenderingContext2D.prototype.drawImage = function(...args) {
                ++window.renderCount; return draw.apply(this, args);
              };
            })()""")
            page.goto(args.base + "/?format=h264&fps=15&quality=75&maxWidth=360&skipUnchanged=1",
                      wait_until="domcontentloaded")
            page.wait_for_function("h264Ready && renderCount > 0", timeout=15000)
            check("H.264 decodes its first frame through WebCodecs",
                  pixels(page)["nonblack"] > 0 and pixels(page)["width"] == 360)
            page.evaluate("setMaxWidth(480)")
            page.wait_for_function("h264Ready && cvs.width === 480", timeout=15000)
            check("H.264 reconfigures its decoder after a size change",
                  pixels(page)["nonblack"] > 0)
            page.evaluate("window.beforeJpeg = renderCount; setCodec('jpeg',75)")
            page.wait_for_function("streamFormat === 'jpeg' && !h264Dec && renderCount > beforeJpeg",
                                   timeout=15000)
            check("H.264 switches to JPEG and keeps rendering", pixels(page)["nonblack"] > 0)
            page.evaluate("window.beforeH264 = renderCount; setCodec('h264',75)")
            page.wait_for_function("streamFormat === 'h264' && h264Ready && renderCount > beforeH264",
                                   timeout=15000)
            check("JPEG switches back to H.264 and decodes a new keyframe",
                  pixels(page)["nonblack"] > 0)
            page.wait_for_timeout(1500)
            frames_before = len(frames)
            page.evaluate("window.beforeRefresh = renderCount; sendStream({t:'refresh'})")
            page.wait_for_function("renderCount > beforeRefresh", timeout=15000)
            page.wait_for_timeout(500)
            check("H.264 refresh drains and displays an output while stop detection is enabled",
                  page.evaluate("h264Ready && skipUnchanged && renderCount > beforeRefresh"))
            print("H.264 frames received after refresh:", len(frames) - frames_before)
            shot = Path(args.shots + "-h264.png").resolve()
            page.screenshot(path=str(shot))
            print("Screenshot:", shot)
            page.evaluate("fps = 0; stopStream()")
            page.close()
        browser.close()

    for error in errors:
        print("PAGE ERROR:", error)
    check("no uncaught browser JavaScript errors", not errors)
    print(f"Passed {sum(checks)}/{len(checks)} checks")
    return 0 if all(checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
