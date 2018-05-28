from xml.dom import minidom
from io import BytesIO
from xml.etree.ElementTree import ElementTree, fromstring, tostring


ENTITY_USER_ID = 0
ENTITY_SDDL_ACL = 1
ENTITY_NETWORK_PATH = 2


class GPNoParserException(Exception):
    pass

class GPGeneralizeException(Exception):
    pass


def entity_type_to_string(ent_type):
    type_str = None

    if ent_type == ENTITY_USER_ID:
        type_str = "USER_ID"
    elif ent_type == ENTITY_SDDL_ACL:
        type_str = "SDDL_ACL"
    elif ent_type == ENTITY_NETWORK_PATH:
        type_str = "NETWORK_PATH"

    return type_str


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

    def new_xml_entity(self, global_entities, ent_type):
        identifier = str(len(global_entities)).zfill(4)

        type_str = entity_type_to_string(ent_type)

        if type_str is None:
            raise GPGeneralizeException("No such entity type")

        # For formattting reasons, align the length of the entities
        longest = entity_type_to_string(ENTITY_NETWORK_PATH)
        type_str = type_str.center(len(longest), '_')

        return "&SAMBA__{}__{}__;".format(type_str, identifier)

    def generalize_xml(self, root, out_file, global_entities):
        entities = []

        # Locate all user_id and all ACLs
        user_ids = root.findall('.//*[@user_id="TRUE"]')

        for elem in user_ids:
            old_text = elem.text
            if old_text is None or old_text == '':
                continue

            if old_text in global_entities:
                elem.text = global_entities[old_text]
            else:
                elem.text = self.new_xml_entity(global_entities,
                                                ENTITY_USER_ID)

                entities.append((elem.text, old_text))
                global_entities.update([(old_text, elem.text)])

        acls = root.findall('.//*[@acl="TRUE"]')
        for elem in acls:
            old_text = elem.text

            if old_text is None or old_text == '':
                continue

            if old_text in global_entities:
                elem.text = global_entities[old_text]
            else:
                elem.text = self.new_xml_entity(global_entities,
                                                ENTITY_SDDL_ACL)

                entities.append((elem.text, old_text))
                global_entities.update([(old_text, elem.text)])

        share_paths = root.findall('.//*[@network_path="TRUE"]')
        for elem in share_paths:
            old_text = elem.text

            if old_text is None or old_text == '':
                continue

            stripped = old_text.lstrip('\\')
            file_server = stripped.split('\\')[0]

            server_index = old_text.find(file_server)

            remaining = old_text[server_index + len(file_server):]
            old_text = old_text[:server_index] + file_server

            if old_text in global_entities:
                elem.text = global_entities[old_text] + remaining
            else:
                to_put = self.new_xml_entity(global_entities,
                                             ENTITY_NETWORK_PATH)
                elem.text = to_put + remaining

                entities.append((to_put, old_text))
                global_entities.update([(old_text, to_put)])

        # Call any file specific customization of entities
        # (which appear in any subclasses).
        entities.extend(self.custom_entities(root, global_entities))

        output_xml = tostring(root)

        for ent in entities:
            output_xml = output_xml.replace(ent[0].replace('&', '&amp;'), ent[0])

        with open(out_file, 'wb') as f:
            f.write(output_xml)

        #return (entities, identifier)
        return (entities, None)

    def custom_entities(self, root, global_entities):
        # Override this method to do special entity handling
        return []
