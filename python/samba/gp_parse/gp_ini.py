import codecs
import collections

from ConfigParser import ConfigParser
from xml.etree.ElementTree import Element, SubElement
from StringIO import StringIO

from samba.gp_parse import GPParser

# [MS-GPFR] Group Policy Folder Redirection
# [MS-GPSCR] Scripts Extension
class GPIniParser(GPParser):
    ini_conf = None

    def parse(self, contents):
        # Required dict_type in Python 2.7
        self.ini_conf = ConfigParser(dict_type=collections.OrderedDict)
        self.ini_conf.optionxform = str

        self.ini_conf.readfp(StringIO(contents.decode(self.encoding)))

    def write_xml(self, filename):
        with file(filename, 'w') as f:
            root = Element('IniFile')

            for sec_ini in self.ini_conf.sections():
                section = SubElement(root, 'Section')
                section.attrib['name'] = sec_ini
                for key_ini, val_ini in self.ini_conf.items(sec_ini, raw=True):
                    child = SubElement(section, 'Parameter')
                    key = SubElement(child, 'Key')
                    value = SubElement(child, 'Value')
                    key.text = key_ini
                    value.text = val_ini

            self.write_pretty_xml(root, f)

        # from xml.etree.ElementTree import fromstring
        # contents = codecs.open(filename, encoding='utf-8').read()
        # self.load_xml(fromstring(contents))

    def load_xml(self, root):
        # Required dict_type in Python 2.7
        self.ini_conf = ConfigParser(dict_type=collections.OrderedDict)
        self.ini_conf.optionxform = str

        for s in root.findall('Section'):
            section_name = s.attrib['name']
            self.ini_conf.add_section(section_name)

            for param in s.findall('Parameter'):
                key = param.find('Key').text
                value = param.find('Value').text
                if value is None:
                    value = ''
                self.ini_conf.set(section_name, key, value)

    def write_binary(self, filename):
        with codecs.open(filename, 'wb+', self.encoding) as f:
            self.ini_conf.write(f)


class GPTIniParser(GPIniParser):
    encoding = 'utf-8'
