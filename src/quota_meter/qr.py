import sys
from typing import TextIO

import qrcode


def print_qr(data: str, output: TextIO = sys.stdout) -> None:
    qr = qrcode.QRCode(border=1)
    qr.add_data(data)
    qr.make(fit=True)
    qr.print_ascii(out=output, tty=output.isatty())
