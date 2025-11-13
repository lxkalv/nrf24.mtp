import layers.ApplicationLayer as ApplicationLayer


def TX_MODE(config):
    file_content = ApplicationLayer.load_file_bytes(config.file_path_tx)
    if not file_content: return

    




def main():
    config = ApplicationLayer.parse_arguments("Point to Point Mode")
    
    if config.mode == ApplicationLayer.Mode.TX:
        TX_MODE(config)

if __name__ == "__main__":
    main()