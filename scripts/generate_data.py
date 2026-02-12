import csv
import random
import math
import time

def generate_candle(timestamp, open_price, volatility_pct, trend_pct):
    """
    Generate a realistic OHLCV candle.
    volatility_pct: typical range 0.1 ~ 2.0 (percent)
    trend_pct:      drift per candle, e.g. 0.02 = 0.02% upward bias
    """
    # Price movement with trend + noise
    change_pct = random.gauss(trend_pct, volatility_pct) / 100.0
    close_price = open_price * (1.0 + change_pct)
    
    # Intrabar volatility (wicks)
    wick_factor = volatility_pct / 100.0
    high_wick = abs(random.gauss(0, wick_factor * 0.7))
    low_wick = abs(random.gauss(0, wick_factor * 0.7))
    
    high_price = max(open_price, close_price) * (1.0 + high_wick)
    low_price = min(open_price, close_price) * (1.0 - low_wick)
    
    # Volume: base + spike on big moves
    base_volume = random.uniform(5.0, 50.0)
    vol_multiplier = 1.0 + abs(change_pct * 100) * 2.0  # Bigger move = more volume
    volume = base_volume * vol_multiplier
    
    return {
        'timestamp': int(timestamp),
        'open': round(open_price, 0),
        'high': round(high_price, 0),
        'low': round(low_price, 0),
        'close': round(close_price, 0),
        'volume': round(volume, 4)
    }

def generate_dataset(filename, num_candles=2000, start_price=50000000):
    """
    Generate synthetic KRW-BTC 1-minute candle data.
    start_price: 50,000,000 KRW (typical BTC/KRW price)
    
    Regime Schedule:
      0-200:    Quiet ranging (warmup)
      200-600:  Strong uptrend
      600-900:  High volatility chop
      900-1200: Downtrend
      1200-1500: Mean-reverting range
      1500-1700: Breakout rally
      1700-2000: Gradual decline
    """
    start_time = int(time.time()) - (num_candles * 60)
    current_price = start_price
    
    candles = []
    current_time = start_time
    
    for i in range(num_candles):
        # Determine regime parameters
        if i < 200:
            # Warmup: tight range
            vol = 0.15
            trend = 0.0
        elif i < 600:
            # Strong uptrend
            vol = 0.25
            trend = 0.08  # ~0.08% per candle drift up
        elif i < 900:
            # High volatility chop
            vol = 0.80
            trend = 0.0
        elif i < 1200:
            # Downtrend
            vol = 0.35
            trend = -0.06
        elif i < 1500:
            # Mean-reverting range (oscillating)
            vol = 0.20
            # Oscillate around mean
            cycle = math.sin(2 * math.pi * (i - 1200) / 60) * 0.05
            trend = cycle
        elif i < 1700:
            # Breakout rally
            vol = 0.40
            trend = 0.12
        else:
            # Gradual decline
            vol = 0.30
            trend = -0.04
            
        candle = generate_candle(current_time, current_price, vol, trend)
        candles.append(candle)
        current_price = candle['close']
        current_time += 60

    with open(filename, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['timestamp', 'open', 'high', 'low', 'close', 'volume'])
        for c in candles:
            writer.writerow([c['timestamp'], c['open'], c['high'], c['low'], c['close'], c['volume']])

    print(f"[OK] Generated {num_candles} candles -> {filename}")
    print(f"     Start Price: {start_price:,.0f} KRW")
    print(f"     End Price:   {current_price:,.0f} KRW")
    print(f"     Change:      {((current_price/start_price)-1)*100:.2f}%")

if __name__ == "__main__":
    import os
    os.makedirs('data/backtest', exist_ok=True)
    generate_dataset('data/backtest/simulation_2000.csv', num_candles=2000)
