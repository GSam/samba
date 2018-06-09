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

    def build_xml_parameter(self, section_xml, section, key_ini, val_ini):
        child = SubElement(section_xml, 'Parameter')
        key = SubElement(child, 'Key')
        value = SubElement(child, 'Value')
        key.text = key_ini
        value.text = val_ini

        return child

    def load_xml_parameter(self, param_xml, section):
        key = param_xml.find('Key').text
        value = param_xml.find('Value').text
        if value is None:
            value = ''
        self.ini_conf.set(section, key, value)

        return (key, value)

    def build_xml_section(self, root_xml, sec_ini):
        section = SubElement(root_xml, 'Section')
        section.attrib['name'] = sec_ini

        return section

    def load_xml_section(self, section_xml):
        section_name = section_xml.attrib['name']
        self.ini_conf.add_section(section_name)

        return section_name

    def write_xml(self, filename):
        with file(filename, 'w') as f:
            root = Element('IniFile')

            for sec_ini in self.ini_conf.sections():
                section = self.build_xml_section(root, sec_ini)

                for key_ini, val_ini in self.ini_conf.items(sec_ini, raw=True):
                    self.build_xml_parameter(section, sec_ini, key_ini,
                                             val_ini)

            self.write_pretty_xml(root, f)

        # from xml.etree.ElementTree import fromstring
        # contents = codecs.open(filename, encoding='utf-8').read()
        # self.load_xml(fromstring(contents))

    def load_xml(self, root):
        # Required dict_type in Python 2.7
        self.ini_conf = ConfigParser(dict_type=collections.OrderedDict)
        self.ini_conf.optionxform = str

        for s in root.findall('Section'):
            section_name = self.load_xml_section(s)

            for param in s.findall('Parameter'):
                self.load_xml_parameter(param, section_name)

    def write_binary(self, filename):
        with codecs.open(filename, 'wb+', self.encoding) as f:
            self.ini_conf.write(f)


class GPTIniParser(GPIniParser):
    encoding = 'utf-8'
