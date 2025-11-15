from nrf24_mtp.utils import Logger

print("Coloring functions test:")
print("RED():", Logger.RED("This is a red message"))
print("GREEN():", Logger.GREEN("This is a green message"))
print("YELLOW():", Logger.YELLOW("This is a yellow message"))
print("BLUE():", Logger.BLUE("This is a blue message"))
print("\n\n")

print("Messaging functions test:")
Logger.ERROR("This is an error message")
Logger.SUCC("This is a success message")
Logger.WARN("This is a warning message")
Logger.INFO("This is an info message")
print("\n\n")

print("Timestamp function test:")
print("Current timestamp:", Logger.timestamp())