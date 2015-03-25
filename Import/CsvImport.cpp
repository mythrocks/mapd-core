#include "CsvImport.h"
#include "../StringDictionary/StringDictionary.h"

#include <iostream>
#include <fstream>
#include <boost/filesystem.hpp>
#include <glog/logging.h>
#include "../Shared/measure.h"

#include <cstdio>
#include <cstdlib>
#include <list>

static int64_t total_csv_parse_time_us = 0;
static int64_t total_insert_time_ms = 0;

MapDMeta::MapDMeta(const std::string table_name, const std::string& base_data_path)
  : table_name_(table_name)
  , table_id_(-1)
  , base_data_path_(base_data_path) {
  CHECK(boost::filesystem::exists(base_data_path_));
  const auto system_db_file = boost::filesystem::path(base_data_path_) / "mapd_catalogs" / "mapd";
  CHECK(boost::filesystem::exists(system_db_file));
  const auto data_path = boost::filesystem::path(base_data_path_) / "mapd_data";
  data_mgr_.reset(new Data_Namespace::DataMgr(data_path.string()));
  Catalog_Namespace::SysCatalog sys_cat(base_data_path_, *data_mgr_);
  Catalog_Namespace::UserMetadata user_meta;
  CHECK(sys_cat.getMetadataForUser(user_, user_meta));
  CHECK_EQ(user_meta.passwd, pass_);
  Catalog_Namespace::DBMetadata db_meta;
  CHECK(sys_cat.getMetadataForDB(db_name_, db_meta));
  CHECK(user_meta.isSuper || user_meta.userId == db_meta.dbOwner);
  cat_ = new Catalog_Namespace::Catalog(base_data_path_, user_meta, db_meta, *data_mgr_);
  td_ = cat_->getMetadataForTable(table_name_);
  CHECK(td_);
  table_id_ = td_->tableId;
  col_descriptors_ = cat_->getAllColumnMetadataForTable(table_id_);
}

const std::list<const ColumnDescriptor*>& MapDMeta::getColumnDescriptors() const {
  return col_descriptors_;
}

int MapDMeta::getTableId() const {
  return table_id_;
}

const TableDescriptor* MapDMeta::getTableDesc() const {
  return td_;
}

int MapDMeta::getDbId() const {
  return cat_->get_currentDB().dbId;
}

Data_Namespace::DataMgr* MapDMeta::getDataMgr() const {
  auto& dm = cat_->get_dataMgr();
  return &dm;
}

StringDictionary* MapDMeta::getStringDict(const int col_id) const {
  for (const auto cd : col_descriptors_) {
    if (cd->columnId == col_id && cd->columnType.is_string() && cd->columnType.get_compression() == kENCODING_DICT) {
      const auto dd = cat_->getMetadataForDict(cd->columnType.get_comp_param());
      CHECK(dd);
      return new StringDictionary(dd->dictFolderPath);
    }
  }
  return nullptr;
}

namespace {

class TypedImportBuffer : boost::noncopyable {
public:
  TypedImportBuffer(
    const ColumnDescriptor* col_desc,
    StringDictionary* string_dict)
    : type_(col_desc->columnType.get_type())
    , encoding_(col_desc->columnType.get_compression())
    , string_dict_(string_dict) {
    switch (type_) {
    case kSMALLINT:
      smallint_buffer_ = new std::vector<int16_t>();
      break;
    case kINT:
      int_buffer_ = new std::vector<int32_t>();
      break;
    case kBIGINT:
      bigint_buffer_ = new std::vector<int64_t>();
      break;
    case kFLOAT:
      float_buffer_ = new std::vector<float>();
      break;
    case kDOUBLE:
      double_buffer_ = new std::vector<double>();
      break;
    case kTEXT:
      string_buffer_ = new std::vector<std::string>();
      if (encoding_ == kENCODING_DICT) {
        string_dict_buffer_ = new std::vector<int32_t>();
      }
      break;
    case kTIME:
    case kTIMESTAMP:
    case kDATE:
      time_buffer_ = new std::vector<time_t>();
      break;
    default:
      CHECK(false);
    }
  }

  ~TypedImportBuffer() {
    switch (type_) {
    case kSMALLINT:
      delete smallint_buffer_;
      break;
    case kINT:
      delete int_buffer_;
      break;
    case kBIGINT:
      delete bigint_buffer_;
      break;
    case kFLOAT:
      delete float_buffer_;
      break;
    case kDOUBLE:
      delete double_buffer_;
      break;
    case kTEXT:
      delete string_buffer_;
      if (encoding_ == kENCODING_DICT) {
        delete string_dict_buffer_;
      }
      break;
    case kTIME:
    case kTIMESTAMP:
    case kDATE:
      delete time_buffer_;
      break;
    default:
      CHECK(false);
    }
  }

  void addSmallint(const int16_t v) {
    CHECK_EQ(kSMALLINT, type_);
    smallint_buffer_->push_back(v);
  }

  void addInt(const int32_t v) {
    CHECK_EQ(kINT, type_);
    int_buffer_->push_back(v);
  }

  void addBigint(const int64_t v) {
    CHECK_EQ(kBIGINT, type_);
    bigint_buffer_->push_back(v);
  }

  void addFloat(const float v) {
    CHECK_EQ(kFLOAT, type_);
    float_buffer_->push_back(v);
  }

  void addDouble(const double v) {
    CHECK_EQ(kDOUBLE, type_);
    double_buffer_->push_back(v);
  }

  void addString(const std::string& v) {
    CHECK_EQ(kTEXT, type_);
    string_buffer_->push_back(v);
  }

  void addTime(const time_t v) {
    CHECK(type_ == kTIME || type_ == kTIMESTAMP || type_ == kDATE);
    time_buffer_->push_back(v);
  }

  void addDictEncodedString(const std::string& v) {
    CHECK_EQ(kTEXT, type_);
    CHECK(string_dict_);
    string_dict_buffer_->push_back(string_dict_->getOrAdd(v));
  }

  SQLTypes getType() const {
    return type_;
  }

  EncodingType getEncoding() const {
    return encoding_;
  }

  int8_t* getAsBytes() const {
    switch (type_) {
    case kSMALLINT:
      return reinterpret_cast<int8_t*>(&((*smallint_buffer_)[0]));
    case kINT:
      return reinterpret_cast<int8_t*>(&((*int_buffer_)[0]));
    case kBIGINT:
      return reinterpret_cast<int8_t*>(&((*bigint_buffer_)[0]));
    case kFLOAT:
      return reinterpret_cast<int8_t*>(&((*float_buffer_)[0]));
    case kDOUBLE:
      return reinterpret_cast<int8_t*>(&((*double_buffer_)[0]));
    case kTIME:
    case kTIMESTAMP:
    case kDATE:
      return reinterpret_cast<int8_t*>(&((*time_buffer_)[0]));
    default:
      CHECK(false);
    }
  }

  std::vector<std::string>* getStringBuffer() const {
    return string_buffer_;
  }

  int8_t* getStringDictBuffer() const {
    return reinterpret_cast<int8_t*>(&((*string_dict_buffer_)[0]));
  }

  void flush() {
    switch (type_) {
    case kSMALLINT: {
      std::vector<int16_t> empty;
      smallint_buffer_->swap(empty);
      break;
    }
    case kINT: {
      std::vector<int32_t> empty;
      int_buffer_->swap(empty);
      break;
    }
    case kBIGINT: {
      std::vector<int64_t> empty;
      bigint_buffer_->swap(empty);
      break;
    }
    case kFLOAT: {
      std::vector<float> empty;
      float_buffer_->swap(empty);
      break;
    }
    case kDOUBLE: {
      std::vector<double> empty;
      double_buffer_->swap(empty);
      break;
    }
    case kTEXT: {
      std::vector<std::string> empty;
      string_buffer_->swap(empty);
      if (encoding_ == kENCODING_DICT) {
        std::vector<int32_t> empty;
        string_dict_buffer_->swap(empty);
      }
      break;
    }
    case kTIME:
    case kTIMESTAMP:
    case kDATE: {
      std::vector<time_t> empty;
      time_buffer_->swap(empty);
      break;
    }
    default:
      CHECK(false);
    }
  }
private:
  union {
    std::vector<int16_t>* smallint_buffer_;
    std::vector<int32_t>* int_buffer_;
    std::vector<int64_t>* bigint_buffer_;
    std::vector<float>* float_buffer_;
    std::vector<double>* double_buffer_;
    std::vector<time_t>* time_buffer_;
    std::vector<std::string>* string_buffer_;
  };
  std::vector<int32_t>* string_dict_buffer_;
  SQLTypes type_;
  EncodingType encoding_;
  std::unique_ptr<StringDictionary> string_dict_;
};

};

CsvImporter::CsvImporter(
    const std::string& table_name,
    const std::string& base_data_path,
    const std::string& file_path,
    const std::string& delim)
  : table_name_(table_name),
    file_path_(file_path)
  , table_meta_(table_name, base_data_path) {
    csv_parser_.set_skip_lines(1);
    csv_parser_.init(file_path.c_str());
    csv_parser_.set_enclosed_char(0, ENCLOSURE_NONE);
    csv_parser_.set_field_term_char(delim[0]);
    csv_parser_.set_line_term_char('\n');
  }

namespace {

void do_import(
    std::vector<std::unique_ptr<TypedImportBuffer>>& import_buffers,
    const size_t row_count,
    Fragmenter_Namespace::InsertData& insert_data,
    Data_Namespace::DataMgr* data_mgr,
    Fragmenter_Namespace::AbstractFragmenter* fragmenter) {
  auto ms = measure<>::execution([&]() {
  {
    decltype(insert_data.data) empty;
    insert_data.data.swap(empty);
  }
  insert_data.numRows = row_count;
  for (const auto& import_buff : import_buffers) {
    DataBlockPtr p;
    if (IS_NUMBER(import_buff->getType()) ||
        IS_TIME(import_buff->getType())) {
      p.numbersPtr = import_buff->getAsBytes();
    } else {
      CHECK_EQ(kTEXT, import_buff->getType());
      auto string_payload_ptr = import_buff->getStringBuffer();
      if (import_buff->getEncoding() == kENCODING_NONE) {
        p.stringsPtr = string_payload_ptr;
      } else {
        CHECK_EQ(kENCODING_DICT, import_buff->getEncoding());
        for (const auto& str : *string_payload_ptr) {
          import_buff->addDictEncodedString(str);
        }
        p.numbersPtr = import_buff->getStringDictBuffer();
      }
    }
    insert_data.data.push_back(p);
  }
  fragmenter->insertData(insert_data);
  data_mgr->checkpoint();
  for (const auto& import_buff : import_buffers) {
    import_buff->flush();
  }
  });
  total_insert_time_ms += ms;
}

}

static csv_row CsvParser_getRow_measured(csv_parser &csvParser) {
  csv_row csvRow;
  auto us = measure<std::chrono::microseconds>::execution([&]() { csvRow = csvParser.get_row(); });
  total_csv_parse_time_us += us;
  return csvRow;
}

void CsvImporter::import() {
  const size_t row_buffer_size { 1000000 };
  const auto col_descriptors = table_meta_.getColumnDescriptors();
  std::ofstream exception_file;
  exception_file.open(file_path_ + ".exception");
  std::vector<std::unique_ptr<TypedImportBuffer>> import_buffers;
  for (const auto col_desc : col_descriptors) {
    import_buffers.push_back(std::unique_ptr<TypedImportBuffer>(new TypedImportBuffer(
      col_desc,
      table_meta_.getStringDict(col_desc->columnId))));
  }
  Fragmenter_Namespace::InsertData insert_data;
  insert_data.databaseId = table_meta_.getDbId();
  insert_data.tableId = table_meta_.getTableId();
  for (const auto col_desc : col_descriptors) {
    insert_data.columnIds.push_back(col_desc->columnId);
  }
  bool has_exception = false;
  size_t row_count = 0;
  while (csv_parser_.has_more_rows()) {
    auto row = CsvParser_getRow_measured(csv_parser_);
    CHECK_EQ(row.size(), col_descriptors.size());
    int col_idx = 0;
    try {
      for (const auto col_desc : col_descriptors) {
        switch (col_desc->columnType.get_type()) {
        case kSMALLINT:
          if (isdigit(row[col_idx][0]) || row[col_idx][0] == '-') {
            import_buffers[col_idx]->addSmallint((int16_t)std::atoi(row[col_idx].c_str()));
          } else
            import_buffers[col_idx]->addSmallint(NULL_SMALLINT);
          break;
        case kINT:
          if (isdigit(row[col_idx][0]) || row[col_idx][0] == '-') {
            import_buffers[col_idx]->addInt(std::atoi(row[col_idx].c_str()));
          } else
            import_buffers[col_idx]->addInt(NULL_INT);
          break;
        case kBIGINT:
          if (isdigit(row[col_idx][0]) || row[col_idx][0] == '-') {
            import_buffers[col_idx]->addBigint(std::atoll(row[col_idx].c_str()));
          } else
            import_buffers[col_idx]->addBigint(NULL_BIGINT);
          break;
        case kFLOAT:
          if (isdigit(row[col_idx][0]) || row[col_idx][0] == '-') {
            import_buffers[col_idx]->addFloat((float)std::atof(row[col_idx].c_str()));
          } else
            import_buffers[col_idx]->addFloat(NULL_FLOAT);
          break;
        case kDOUBLE:
          if (isdigit(row[col_idx][0]) || row[col_idx][0] == '-') {
            import_buffers[col_idx]->addDouble(std::atof(row[col_idx].c_str()));
          } else
            import_buffers[col_idx]->addDouble(NULL_DOUBLE);
          break;
        case kTEXT: {
          import_buffers[col_idx]->addString(row[col_idx]);
          break;
        }
        case kTIME:
        case kTIMESTAMP:
        case kDATE:
          if (isdigit(row[col_idx][0])) {
            SQLTypeInfo ti = col_desc->columnType;
            Datum d = StringToDatum(row[col_idx], ti);
            import_buffers[col_idx]->addTime(d.timeval);
          } else
            import_buffers[col_idx]->addTime(sizeof(time_t) == 4 ? NULL_INT : NULL_BIGINT);
          break;
        default:
          CHECK(false);
        }
        ++col_idx;
      }
      ++row_count;
    } catch (const std::exception&) {
      for (size_t i = 0; i < col_descriptors.size(); i++) {
        if (i > 0)
          exception_file << ",";
        exception_file << row[i];
      }
      exception_file << std::endl;
      has_exception = true;
    }
    if (row_count == row_buffer_size) {
      do_import(import_buffers, row_count, insert_data,
        table_meta_.getDataMgr(), table_meta_.getTableDesc()->fragmenter);
      row_count = 0;
    }
  }
  if (row_count > 0) {
    do_import(import_buffers, row_count, insert_data,
      table_meta_.getDataMgr(), table_meta_.getTableDesc()->fragmenter);
  }
  std::cout << "Total CSV Parse Time: " << (double)total_csv_parse_time_us/1000000.0 << " Seconds.  Total Insert Time: " << (double)total_insert_time_ms/1000.0 << " Seconds." << std::endl;
  exception_file.close();
  if (has_exception) {
    std::cout << "There were exceptions in the import.  See " + file_path_ + ".exception for the offending rows." << std::endl;
  }
}

CsvImporter::~CsvImporter() {
}
