with open("deltauno.log", "r", encoding="utf-8") as f:
    buys = [line.strip() for line in f if "FILLED BUY:" in line]
    sells = [line.strip() for line in f if "FILLED SELL:" in line]

print(f"Total Buys: {len(buys)}, Total Sells: {len(sells)}")
for i in range(len(sells)):
    b = buys[i] if i < len(buys) else "MISSING BUY"
    s = sells[i]
    print(f"{i+1:2d}: {b}")
    print(f"    {s}")
