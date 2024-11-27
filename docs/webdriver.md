Title: WebDriver support

# WebDriver support

Cog allows remote testing through the WebDriver interface. With Selenium python
bindings, you can use the `selenium.webdriver.remote.WebDriver` class to run
the tests on the target device directly from a host computer.

## Requirements

* `cog` and `WPEWebDriver` installed on the target devices.
* Selenium python bindings on the computer with the test scripts.

## Launching the WebDriver

For example, on a meta-webkit RPi3 image:

```shell
$ export WAYLAND_DISPLAY=wayland-0
$ export XDG_RUNTIME_DIR=/run/user/0
# host=all to allow requests from other machines
$ WPEWebDriver --port=8088 --host=all
```

## Connecting to the driver

In the python script:

```python
from selenium import webdriver

options = webdriver.WPEWebKitOptions()
options.capabilities.clear()
options.binary_location = "/usr/bin/cog"
options.add_argument("--automation")
options.add_argument("--platform=wl")

driver = webdriver.Remote(
    command_executor="http://YOUR_DEVICE_IP:8088",
    options=options,
)
```

## Using the driver

```python
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.common.by import By
from selenium.webdriver.common.actions.action_builder import ActionBuilder

driver.get("https://lichess.org/analysis")
board = WebDriverWait(driver, 10).until(
    EC.presence_of_element_located((By.CLASS_NAME, "cg-wrap"))
)

square_size = board.size["height"] / 8
# ...
actions = ActionBuilder(driver)
pointer = actions.pointer_action
pointer.move_to(board, x, y)
pointer.click()
actions.perform()
```
