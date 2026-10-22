"""Script to verify battery dimensions and CAD pocket clearance for MIphoto."""


def check_battery_tolerances(
    length_mm: float,
    width_mm: float,
    thickness_mm: float,
) -> bool:
    """Validate physical battery dimensions against CAD retention pocket."""
    pocket_x_span: float = 30.0  # X in [-4, +26] -> 30.0 mm
    pocket_y_span: float = 40.0  # Y in [-18, +22] -> 40.0 mm
    pocket_z_span: float = 5.8  # 5.0 mm nominal + 0.8 mm EVA cushion

    valid_x: bool = width_mm <= pocket_x_span
    valid_y: bool = length_mm <= pocket_y_span
    valid_z: bool = thickness_mm <= pocket_z_span
    return valid_x and valid_y and valid_z


if __name__ == "__main__":
    is_fit: bool = check_battery_tolerances(40.0, 30.0, 5.0)
    print(f"Battery CAD Fit Validated: {is_fit}")
