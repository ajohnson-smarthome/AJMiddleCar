# generated from contract/car-api.json by tools/gen_contract.py - do not edit

PROTO = 1
DEVICE = 'ajmiddlecar'
NETWORK = {'ssid': 'AJMiddleCar', 'password': 'drive1234', 'host': '192.168.4.1'}
RT = {'port': 4210, 'max_datagram': 96, 'command_hz': 10, 'telemetry_hz': 5, 'watchdog_ms': 300}

DOMAINS = {   '/ramp': {   'key': 'ramp',
                 'defaults': {'ramp_ms': 300},
                 'fields': [   {   'name': 'ramp_ms',
                                   'type': 'int',
                                   'min': 0,
                                   'max': 2000,
                                   'default': 300,
                                   'doc': 'time from zero to full scale in ms; 0 disables the '
                                          'ramp'}]},
    '/trim': {   'key': 'trim',
                 'defaults': {'trim_pct': 0},
                 'fields': [   {   'name': 'trim_pct',
                                   'type': 'int',
                                   'min': -30,
                                   'max': 30,
                                   'default': 0,
                                   'doc': 'percentage by which the faster side is slowed'}]},
    '/recover': {   'key': 'recover',
                    'defaults': {'enabled': True, 'window_ms': 5000},
                    'fields': [   {   'name': 'enabled',
                                      'type': 'bool',
                                      'default': True,
                                      'doc': 'retrace on unexpected silence; when false the '
                                             'car stops instead'},
                                  {   'name': 'window_ms',
                                      'type': 'int',
                                      'min': 1000,
                                      'max': 10000,
                                      'default': 5000,
                                      'doc': 'how far back the breadcrumb history reaches'}]},
    '/wheel': {   'key': 'wheel',
                  'defaults': {'diameter_mm': 65, 'ppr': 11, 'gear_x100': 2100, 'quad': 4},
                  'fields': [   {   'name': 'diameter_mm',
                                    'type': 'int',
                                    'min': 20,
                                    'max': 150,
                                    'default': 65,
                                    'doc': 'wheel diameter in mm'},
                                {   'name': 'ppr',
                                    'type': 'int',
                                    'min': 1,
                                    'max': 1000,
                                    'default': 11,
                                    'doc': 'encoder pulses per motor-shaft revolution, one '
                                           'channel'},
                                {   'name': 'gear_x100',
                                    'type': 'int',
                                    'min': 100,
                                    'max': 30000,
                                    'default': 2100,
                                    'doc': 'gear ratio times 100; 1:21 is 2100'},
                                {   'name': 'quad',
                                    'type': 'enum',
                                    'values': [1, 2, 4],
                                    'default': 4,
                                    'doc': 'quadrature edge multiplier'}]},
    '/dims': {   'key': 'dims',
                 'defaults': {'track_mm': 130, 'wheelbase_mm': 210},
                 'fields': [   {   'name': 'track_mm',
                                   'type': 'int',
                                   'min': 60,
                                   'max': 300,
                                   'default': 130,
                                   'doc': 'lateral distance between left and right wheel '
                                          'centres'},
                               {   'name': 'wheelbase_mm',
                                   'type': 'int',
                                   'min': 90,
                                   'max': 360,
                                   'default': 210,
                                   'doc': 'longitudinal distance between front and rear wheel '
                                          'centres'}]}}


def validate(path, body):
    """Return (True, "") or (False, reason). Mirrors the firmware exactly."""
    domain = DOMAINS.get(path)
    if domain is None:
        return False, f"unknown endpoint {path}"
    for f in domain["fields"]:
        name = f["name"]
        if name not in body:
            return False, f"missing {name}"
        v = body[name]
        if f["type"] == "bool":
            if not isinstance(v, bool):
                return False, f"{name} must be a boolean"
            continue
        # bool is a subclass of int in Python, so {"ramp_ms": true} would sneak
        # past a plain isinstance check. The firmware's cJSON_IsNumber does not
        # accept a JSON boolean, so neither does this.
        if isinstance(v, bool) or not isinstance(v, int):
            return False, f"{name} must be an integer"
        if f["type"] == "enum":
            if v not in f["values"]:
                return False, f"{name} must be one of {f['values']}"
        elif not (f["min"] <= v <= f["max"]):
            return False, f"{name} must be {f['min']}..{f['max']}"
    return True, ""
