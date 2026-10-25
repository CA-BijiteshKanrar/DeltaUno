/**
 * ============================================================================
 * DeltaUno Live Paper Trading — Google Sheets Real-Time Webhook Bridge
 * ============================================================================
 * SETUP (Takes 30 Seconds — Zero GCP Service Account Required):
 * 1. Open a new Google Sheet (https://sheets.new)
 * 2. Click Extensions -> Apps Script
 * 3. Replace the default code with this file and click Save (Ctrl+S)
 * 4. Click Deploy -> New deployment -> Select type: "Web app"
 *    - Execute as: "Me"
 *    - Who has access: "Anyone"
 * 5. Copy the resulting Web App URL (https://script.google.com/macros/s/.../exec)
 * 6. Set it as GOOGLE_SHEETS_WEBHOOK_URL in:
 *    - config/live_broker.env (for local runs)
 *    - GitHub Repo Settings -> Secrets and variables -> Actions (for GitHub Actions)
 * ============================================================================
 */

function ensureSheet_(ss, name, headers) {
  var sheet = ss.getSheetByName(name);
  if (!sheet) {
    sheet = ss.insertSheet(name);
  }
  if (headers && sheet.getLastRow() === 0) {
    sheet.appendRow(headers);
    var headerRange = sheet.getRange(1, 1, 1, headers.length);
    headerRange.setFontWeight("bold")
               .setBackground("#1a237e")
               .setFontColor("#ffffff");
    sheet.setFrozenRows(1);
  }
  return sheet;
}

function doPost(e) {
  var lock = LockService.getScriptLock();
  lock.tryLock(10000);
  try {
    var payload = JSON.parse(e.postData.contents);
    var ss = SpreadsheetApp.getActiveSpreadsheet();
    var eventType = payload.event || "HEARTBEAT";

    // 1. Ensure all 3 tabs exist
    var dashSheet = ensureSheet_(ss, "Live_Dashboard", null);
    var tradesSheet = ensureSheet_(ss, "Live_Trades", [
      "Timestamp (IST)", "Source", "Track", "Side", "Symbol",
      "Qty", "Fill Price (INR)", "Stop Loss (INR)", "Take Profit (INR)",
      "Gross PnL (INR)", "Taxes (INR)", "Net PnL (INR)", "Cash Balance (INR)"
    ]);
    var summarySheet = ensureSheet_(ss, "Daily_Summary", [
      "Date (IST)", "Source", "Provider", "Ticks Processed",
      "Track A Equity (INR)", "Track A Net PnL (INR)", "Track A Return (%)", "Track A Trades", "Track A Win Rate (%)",
      "Track B Equity (INR)", "Track B Net PnL (INR)", "Track B Return (%)", "Track B Trades", "Track B Win Rate (%)"
    ]);

    // 2. Handle real-time trade fill event
    if (eventType === "TRADE_FILL") {
      tradesSheet.appendRow([
        payload.timestamp_ist || "",
        payload.source || "LOCAL",
        payload.track || "",
        payload.side || "",
        payload.symbol || "",
        payload.qty || 0,
        payload.price || 0,
        payload.sl || "",
        payload.tp || "",
        payload.gross_pnl !== undefined ? payload.gross_pnl : "",
        payload.taxes !== undefined ? payload.taxes : "",
        payload.net_pnl !== undefined ? payload.net_pnl : "",
        payload.balance || 0
      ]);
    }

    // 3. Handle live dashboard update (on heartbeat, trade fill, or test)
    if (payload.dashboard) {
      var d = payload.dashboard;
      dashSheet.clearContents();
      var rows = [
        ["DELTAUNO DUAL-MODE LIVE NSE PAPER TRADING DASHBOARD", "", "", "", "", ""],
        ["Last Updated (IST)", d.timestamp_ist || "", "Execution Host", d.source || "LOCAL", "Live Feed Provider", d.provider || ""],
        ["Session Status", d.status || "RUNNING", "Total Ticks Streamed", d.ticks || 0, "Trading Mode", d.mode || "both"],
        ["", "", "", "", "", ""],
        ["STRATEGY TRACK COMPARISON (LIVE)", "Track A: High-Yield Compounder", "Track B: L2 Microstructure Harvester", "", "", ""],
        ["Live Cash / Equity (INR)", d.comp_equity || 10000, d.harv_equity || 10000, "", "", ""],
        ["Realized Net PnL (INR)", d.comp_pnl || 0, d.harv_pnl || 0, "", "", ""],
        ["Net Return (%)", d.comp_ret_pct || "0.00%", d.harv_ret_pct || "0.00%", "", "", ""],
        ["Closed Trades (Wins / Total)", d.comp_trades || "0 / 0", d.harv_trades || "0 / 0", "", "", ""],
        ["Open Position", d.comp_open_pos || "FLAT", d.harv_open_pos || "FLAT", "", "", ""],
        ["", "", "", "", "", ""],
        ["LIVE NSE 5-INSTRUMENT L2 ORDER BOOK SNAPSHOT", "", "", "", "", ""],
        ["Symbol", "Token", "LTP (INR)", "Best Bid (Qty)", "Best Ask (Qty)", "Circuit Band (LC - UC)"]
      ];

      var quotes = d.quotes || [];
      for (var i = 0; i < quotes.length; i++) {
        var q = quotes[i];
        rows.push([
          q.symbol || "",
          q.token || "",
          q.ltp || 0,
          q.bid_str || "",
          q.ask_str || "",
          q.circuit_str || ""
        ]);
      }

      dashSheet.getRange(1, 1, rows.length, 6).setValues(rows);
      dashSheet.getRange(1, 1, 1, 6).setFontWeight("bold").setBackground("#0d47a1").setFontColor("#ffffff");
      dashSheet.getRange(5, 1, 1, 3).setFontWeight("bold").setBackground("#283593").setFontColor("#ffffff");
      dashSheet.getRange(13, 1, 1, 6).setFontWeight("bold").setBackground("#1b5e20").setFontColor("#ffffff");
    }

    // 4. Handle EOD Daily Summary
    if (eventType === "DAILY_SUMMARY" && payload.summary) {
      var s = payload.summary;
      summarySheet.appendRow([
        s.date_ist || "",
        s.source || "LOCAL",
        s.provider || "",
        s.ticks || 0,
        s.comp_equity || 10000,
        s.comp_pnl || 0,
        s.comp_ret_pct || 0,
        s.comp_trades || 0,
        s.comp_win_rate || 0,
        s.harv_equity || 10000,
        s.harv_pnl || 0,
        s.harv_ret_pct || 0,
        s.harv_trades || 0,
        s.harv_win_rate || 0
      ]);
    }

    return ContentService.createTextOutput(JSON.stringify({ status: "ok", event: eventType }))
                         .setMimeType(ContentService.MimeType.JSON);
  } catch (err) {
    return ContentService.createTextOutput(JSON.stringify({ status: "error", message: String(err) }))
                         .setMimeType(ContentService.MimeType.JSON);
  } finally {
    lock.releaseLock();
  }
}
