import time

from nrf24_mtp.utils import ProgressBar


print("Progress Bar Demo:")
wait_t = 1e-4
limit  = 10_000
bar    = ProgressBar.ProgressBar(limit)
for i in range(limit):
    bar.update(i, "computing serious stuff...")
    time.sleep(wait_t)
bar.finish("computation done!")
print("\n\n")

print("Status Bar Demo:")
wait_t   = 2
bar      = ProgressBar.ProgressBar()
statuses = ["INFO", "WARN", "SUCC", "ERROR"]
for status in statuses:
    bar.status(f"this is a {status} message", status)
    time.sleep(wait_t)