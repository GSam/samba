from xml.dom import minidom
from io import BytesIO
from xml.etree.ElementTree import ElementTree

class GPNoParserException(Exception):
    pass

# [MS-GPIPSEC] (LDAP)
# [MS-GPDPC] Deployed Printer Connections (LDAP)
# [MS-GPPREF] Preferences Extension (XML)
# [MS-GPWL] Wireless/Wired Protocol Extension (LDAP)
class GPParser(object):
    encoding = 'utf-16'
    output_encoding = 'utf-8'

    def parse(self, contents):
        pass

    def write_xml(self, filename):
        with file(filename, 'w') as f:
            f.write('<?xml version="1.0" encoding="utf-8"?><UnknownFile/>')

    def load_xml(self, filename):
        pass

    def write_binary(self, filename):
        raise GPNoParserException("This file has no parser available.")

    def write_pretty_xml(self, xml_element, handle):
        # Add the xml header as well as format it nicely.
        # ElementTree doesn't have a pretty-print, so use minidom.

        et = ElementTree(xml_element)
        temporary_bytes = BytesIO()
        et.write(temporary_bytes, encoding=self.output_encoding,
                 xml_declaration=True)
        minidom_parsed = minidom.parseString(temporary_bytes.getvalue())
        handle.write(minidom_parsed.toprettyxml(encoding=self.output_encoding))
