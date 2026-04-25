#include <cstring>

#include "dxbc_reader.h"

namespace dxvk {
  
  DxbcTag DxbcReader::readTag() {
    DxbcTag tag;
    this->read(&tag, 4);
    return tag;
  }
  
  
  std::string DxbcReader::readString() {
    std::string result;
    
    while (m_data[m_pos] != '\0')
      result.push_back(m_data[m_pos++]);
    
    m_pos++;
    return result;
  }
  
  
  void DxbcReader::read(void* dst, size_t n) {
    if (m_pos + n > m_size)
      dxvk::DxvkError::abort_ositok("DXVK throw");
    std::memcpy(dst, m_data + m_pos, n);
    m_pos += n;
  }
  
  
  void DxbcReader::skip(size_t n) {
    if (m_pos + n > m_size)
      dxvk::DxvkError::abort_ositok("DXVK throw");
    m_pos += n;
  }
  
  
  DxbcReader DxbcReader::clone(size_t pos) const {
    if (pos > m_size)
      dxvk::DxvkError::abort_ositok("DXVK throw");
    return DxbcReader(m_data + pos, m_size - pos);
  }
  
  
  DxbcReader DxbcReader::resize(size_t size) const {
    if (size > m_size)
      dxvk::DxvkError::abort_ositok("DXVK throw");
    return DxbcReader(m_data, size, m_pos);
  }
  
  
  void DxbcReader::store(std::ostream&& stream) const {
    stream.write(m_data, m_size);
  }
  
}