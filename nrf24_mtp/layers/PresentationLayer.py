"""
##### Presentation Layer
Prepares the data. TODO: finish this comment :P

##### Diagram
```
IO
                ┌────────────────────┐
 FILE BYTES <─> │                    │ <─> COMPRESSED PAGES
                │ PRESENTATION LAYER │
                │                    │
                └────────────────────┘
```

##### Inputs
- RADIO CONFIGURATION: Configuration parameters for the radio object
- FILE TO TRANSMIT*: Path to a file to be transmitted (only for TX mode)
- BYTES TO STORE*: Bytes received to be stored in a file (only for RX mode)

##### Outputs
- RADIO OBJECT: Configured radio object ready to transmit/receive data
- BYTES TO TRANSMIT*: Bytes read from the file to be transmitted (only for TX mode)
- FILE TO STORE*: Path to the file where the received bytes will be stored (only for RX mode)
"""