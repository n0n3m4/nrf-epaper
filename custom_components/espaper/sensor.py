"""Battery, from the panel's VDDH reading in its advertisement.

Firmware that does not advertise it leaves both sensors unknown.
"""

from __future__ import annotations

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.const import PERCENTAGE, EntityCategory, UnitOfElectricPotential
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import EPaperConfigEntry

# Calibration knobs: the cell's empty and full voltages.
# ponytail: linear LiPo curve, swap for a lookup table if % reads wrong mid-range.
BATTERY_EMPTY_MV = 3300
BATTERY_FULL_MV = 4200


async def async_setup_entry(
    hass: HomeAssistant,
    entry: EPaperConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    c = entry.runtime_data
    async_add_entities([EPaperBattery(c, "battery"), EPaperVoltage(c, "voltage")])


class _EPaperSensor(SensorEntity):
    _attr_has_entity_name = True
    _attr_should_poll = False
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, coordinator, key: str) -> None:
        self.coordinator = coordinator
        self._attr_translation_key = key
        self._attr_unique_id = f"{coordinator.address}_{key}"
        self._attr_device_info = coordinator.device_info

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.async_on_remove(
            self.coordinator.async_add_listener(self.async_write_ha_state)
        )


class EPaperBattery(_EPaperSensor):
    _attr_device_class = SensorDeviceClass.BATTERY
    _attr_native_unit_of_measurement = PERCENTAGE

    @property
    def native_value(self) -> int | None:
        if (mv := self.coordinator.battery_mv) is None:
            return None
        pct = (mv - BATTERY_EMPTY_MV) * 100 // (BATTERY_FULL_MV - BATTERY_EMPTY_MV)
        return max(0, min(100, pct))


class EPaperVoltage(_EPaperSensor):
    _attr_device_class = SensorDeviceClass.VOLTAGE
    _attr_native_unit_of_measurement = UnitOfElectricPotential.MILLIVOLT
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    @property
    def native_value(self) -> int | None:
        return self.coordinator.battery_mv
