from flask import Flask, request, jsonify
import csv, os, joblib
import numpy as np
from datetime import datetime
from collections import deque
from dotenv import load_dotenv

load_dotenv()

app = Flask(__name__)

# ── Paths ─────────────────────────────────────────────────────────────────────
CSV_FILE   = os.getenv('CSV_FILE')
MODEL_PATH = os.getenv('MODEL_PATH')

# ── Model config ──────────────────────────────────────────────────────────────
INPUT_WINDOW     = 3
FORECAST_HORIZON = 1    # 1 step = 2 minutes ahead
N_FEATURES       = 9    # pm25, pm10, temp, hum, decimal_hour, is_weekday, lag1-3

# ── Load model ────────────────────────────────────────────────────────────────
print(f'[STARTUP] Loading model from {MODEL_PATH} ...')
try:
    model = joblib.load(MODEL_PATH)
    print(f'[STARTUP] Model loaded: {type(model).__name__}')
except Exception as e:
    model = None
    print(f'[STARTUP] WARNING — could not load model: {e}')

# ── Rolling buffer ────────────────────────────────────────────────────────────
reading_buffer = deque(maxlen=INPUT_WINDOW)

# ── US AQI PM2.5 Breakpoints (EPA) ───────────────────────────────────────────
# Source: EPA AQI Technical Assistance Document (40 CFR Part 58 Appendix G)
# Columns: AQI_low, AQI_high, PM25_low (µg/m³), PM25_high (µg/m³), category
US_AQI_PM25 = [
    (  0,  50,   0.0,  12.0,  'Good'),
    ( 51, 100,  12.1,  35.4,  'Moderate'),
    (101, 150,  35.5,  55.4,  'Unhealthy for Sensitive Groups'),
    (151, 200,  55.5, 150.4,  'Unhealthy'),
    (201, 300, 150.5, 250.4,  'Very Unhealthy'),
    (301, 500, 250.5, 500.4,  'Hazardous'),
]

AQI_HEALTH_MSG = {
    'Good'                           : 'Air quality is satisfactory.',
    'Moderate'                       : 'Acceptable; sensitive people may be affected.',
    'Unhealthy for Sensitive Groups' : 'Sensitive groups should reduce outdoor activity.',
    'Unhealthy'                      : 'Everyone may begin to experience health effects.',
    'Very Unhealthy'                 : 'Health alert — everyone may be seriously affected.',
    'Hazardous'                      : 'Health emergency — avoid all outdoor activity.',
}

# Short labels for 16x2 LCD (16 chars max)
AQI_SHORT = {
    'Good'                           : 'Good',
    'Moderate'                       : 'Moderate',
    'Unhealthy for Sensitive Groups' : 'Sensitive Grp',
    'Unhealthy'                      : 'Unhealthy',
    'Very Unhealthy'                 : 'Very Unhealthy',
    'Hazardous'                      : 'Hazardous',
}


def pm25_to_aqi(concentration):
    """
    Convert PM2.5 (µg/m³) to US AQI using the EPA linear interpolation formula:

        AQI = ((AQI_high - AQI_low) / (PM25_high - PM25_low))
              * (Cp - PM25_low) + AQI_low

    EPA truncates PM2.5 to 1 decimal place before applying the formula.
    Returns a dict with aqi_value, category, category_short, health_message.
    Returns None if concentration is negative or None.
    """
    if concentration is None or concentration < 0:
        return None

    cp = round(float(concentration), 1)   # EPA: truncate to 1 decimal

    for (aqi_lo, aqi_hi, pm_lo, pm_hi, category) in US_AQI_PM25:
        if pm_lo <= cp <= pm_hi:
            aqi = ((aqi_hi - aqi_lo) / (pm_hi - pm_lo)) * (cp - pm_lo) + aqi_lo
            return {
                'aqi_value'     : round(aqi),
                'category'      : category,
                'category_short': AQI_SHORT[category],
                'health_message': AQI_HEALTH_MSG[category],
            }

    # Above highest breakpoint — clamp to Hazardous max
    return {
        'aqi_value'     : 500,
        'category'      : 'Hazardous',
        'category_short': AQI_SHORT['Hazardous'],
        'health_message': AQI_HEALTH_MSG['Hazardous'],
    }


def compute_features(buf):
    """Build flattened (1, INPUT_WINDOW * N_FEATURES) feature array from buffer."""
    buf_list = list(buf)
    features = []
    for pos, r in enumerate(buf_list):
        lag1 = buf_list[pos - 1]['pm25'] if pos >= 1 else r['pm25']
        lag2 = buf_list[pos - 2]['pm25'] if pos >= 2 else r['pm25']
        lag3 = buf_list[pos - 3]['pm25'] if pos >= 3 else r['pm25']
        features.extend([
            r['pm25'], r['pm10'], r['temp'], r['hum'],
            r['decimal_hour'], r['is_weekday'],
            lag1, lag2, lag3,
        ])
    return np.array(features).reshape(1, -1)


# ── Routes ────────────────────────────────────────────────────────────────────
@app.route('/')
def home():
    return (
        f"Server running | "
        f"Model: {'loaded (' + type(model).__name__ + ')' if model else 'NOT LOADED'} | "
        f"Buffer: {len(reading_buffer)}/{INPUT_WINDOW}"
    )


@app.route('/kirim-data', methods=['POST'])
def terima_data():
    try:
        data           = request.get_json()
        pm25           = float(data.get('pm25'))
        pm10           = float(data.get('pm10'))
        temp           = float(data.get('temp'))
        hum            = float(data.get('hum'))
        now            = datetime.now()
        decimal_hour   = now.hour + now.minute / 60
        is_weekday     = 1 if now.weekday() < 5 else 0
        timestamp_note = now.strftime('%x %X')

        # Current AQI from actual reading
        current_aqi = pm25_to_aqi(pm25)

        # Save to CSV
        file_baru = not os.path.exists(CSV_FILE)
        with open(CSV_FILE, mode='a', newline='') as f:
            writer = csv.writer(f)
            if file_baru:
                writer.writerow(['timestamp', 'time_hour', 'time_minute',
                                  'time_second', 'pm25', 'pm10', 'temp', 'hum'])
            writer.writerow([now.strftime('%m/%d/%y'), now.hour, now.minute,
                              now.second, pm25, pm10, temp, hum])

        # Update buffer
        reading_buffer.append({
            'pm25': pm25, 'pm10': pm10, 'temp': temp, 'hum': hum,
            'decimal_hour': decimal_hour, 'is_weekday': is_weekday,
        })

        # Predict if buffer is full
        predicted_pm25 = None
        predicted_aqi  = None
        buffer_status  = f'{len(reading_buffer)}/{INPUT_WINDOW}'

        if model is not None and len(reading_buffer) == INPUT_WINDOW:
            predicted_pm25 = round(float(model.predict(compute_features(reading_buffer))[0]), 2)
            predicted_aqi  = pm25_to_aqi(predicted_pm25)

        print(
            f'[{timestamp_note}] PM25={pm25} AQI={current_aqi["aqi_value"] if current_aqi else "?"}'
            f' ({current_aqi["category"] if current_aqi else "?"}) | '
            f'PM10={pm10} T={temp} H={hum} | Buffer={buffer_status} | '
            f'PredPM25={predicted_pm25} '
            f'PredAQI={predicted_aqi["aqi_value"] if predicted_aqi else "warmup"}'
        )

        return jsonify({
            'status'       : 'success',
            'time'         : timestamp_note,
            'buffer_status': buffer_status,
            'current': {
                'pm25_ugm3': pm25,
                'aqi'      : current_aqi,
            },
            'prediction': {
                'horizon_minutes': FORECAST_HORIZON * 2,
                'pm25_ugm3'      : predicted_pm25,
                'aqi'            : predicted_aqi,
                'note'           : (
                    f'PM2.5 & AQI predicted {FORECAST_HORIZON * 2} min ahead'
                    if predicted_pm25 is not None
                    else f'Warming up — need {INPUT_WINDOW - len(reading_buffer)} more reading(s)'
                ),
            },
        }), 200

    except Exception as e:
        return jsonify({'status': 'failed', 'error': str(e)}), 400


@app.route('/buffer', methods=['GET'])
def get_buffer():
    return jsonify({
        'buffer_size'  : len(reading_buffer),
        'buffer_needed': INPUT_WINDOW,
        'ready'        : len(reading_buffer) == INPUT_WINDOW,
        'readings'     : list(reading_buffer),
    }), 200


@app.route('/reset-buffer', methods=['POST'])
def reset_buffer():
    reading_buffer.clear()
    return jsonify({'status': 'buffer cleared', 'buffer_size': 0}), 200


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)
