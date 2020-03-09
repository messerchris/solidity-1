/*
	This file is part of solidity.
	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <test/libsolidity/SemanticTest.h>
#include <test/Common.h>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/throw_exception.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <stdexcept>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::util::formatting;
using namespace solidity::frontend::test;
using namespace boost;
using namespace boost::algorithm;
using namespace boost::unit_test;
namespace fs = boost::filesystem;


SemanticTest::SemanticTest(string const& _filename, langutil::EVMVersion _evmVersion):
	SolidityExecutionFramework(_evmVersion)
{
	ifstream file(_filename);
	soltestAssert(file, "Cannot open test contract: \"" + _filename + "\".");
	file.exceptions(ios::badbit);

	std::tie(m_source, m_lineOffset) = parseSourceAndSettingsWithLineNumbers(file);

	if (m_settings.count("compileViaYul"))
	{
		if (m_settings["compileViaYul"] == "also")
		{
			m_validatedSettings["compileViaYul"] = m_settings["compileViaYul"];
			m_runWithYul = true;
			m_runWithoutYul = true;
		}
		else
		{
			m_validatedSettings["compileViaYul"] = "only";
			m_runWithYul = true;
			m_runWithoutYul = false;
		}
		m_settings.erase("compileViaYul");
	}
	if (m_settings.count("ABIEncoderV1Only"))
	{
		if (m_settings["ABIEncoderV1Only"] == "true")
		{
			m_validatedSettings["ABIEncoderV1Only"] = "true";
			m_runWithABIEncoderV1Only = true;
		}
		m_settings.erase("ABIEncoderV1Only");
	}

	if (m_runWithABIEncoderV1Only && solidity::test::CommonOptions::get().useABIEncoderV2)
		m_shouldRun = false;

	if (m_settings.count("revertStrings"))
	{
		auto revertStrings = revertStringsFromString(m_settings["revertStrings"]);
		if (revertStrings)
			m_revertStrings = *revertStrings;
		m_validatedSettings["revertStrings"] = revertStringsToString(m_revertStrings);
		m_settings.erase("revertStrings");
	}

	if (m_settings.count("allowNonExistingFunctions"))
	{
		m_validatedSettings["allowNonExistingFunctions"] = true;
		m_settings.erase("allowNonExistingFunctions");
	}

	parseExpectations(file);
	soltestAssert(!m_tests.empty(), "No tests specified in " + _filename);
}

TestCase::TestResult SemanticTest::run(ostream& _stream, string const& _linePrefix, bool _formatted)
{
	for(bool compileViaYul: set<bool>{!m_runWithoutYul, m_runWithYul})
	{
		reset();
		bool success = true;

		m_compileViaYul = compileViaYul;
		if (compileViaYul)
			AnsiColorized(_stream, _formatted, {BOLD, CYAN}) << _linePrefix << "Running via Yul:" << endl;

		for (auto& test: m_tests)
			test.reset();

		map<string, solidity::test::Address> libraries;

		bool constructed = false;

		for (auto& test: m_tests)
		{
			if (constructed)
			{
				soltestAssert(!test.call().isLibrary, "Libraries have to be deployed before any other call.");
				soltestAssert(!test.call().isConstructor, "Constructor has to be the first function call expect for library deployments.");
			}
			else if (test.call().isLibrary)
			{
				soltestAssert(
					deploy(test.call().signature, 0, {}, libraries) && m_transactionSuccessful,
					"Failed to deploy library " + test.call().signature
				);
				libraries[test.call().signature] = m_contractAddress;
				continue;
			}
			else
			{
				if (test.call().isConstructor)
					deploy("", test.call().value.value, test.call().arguments.rawBytes(), libraries);
				else
					soltestAssert(deploy("", 0, bytes(), libraries), "Failed to deploy contract.");
				constructed = true;
			}

			if (test.call().isConstructor)
			{
				if (m_transactionSuccessful == test.call().expectations.failure)
					success = false;

				test.setFailure(!m_transactionSuccessful);
				test.setRawBytes(bytes());
			}
			else
			{
				bytes output;
				if (test.call().useCallWithoutSignature)
					output = callLowLevel(test.call().arguments.rawBytes(), test.call().value.value);
				else
				{
					soltestAssert(
						m_validatedSettings.count("allowNonExistingFunctions") || m_compiler.methodIdentifiers(m_compiler.lastContractName()).isMember(test.call().signature),
						"The function " + test.call().signature + " is not known to the compiler"
					);

					output = callContractFunctionWithValueNoEncoding(
						test.call().signature,
						test.call().value.value,
						test.call().arguments.rawBytes()
					);
				}

				if ((m_transactionSuccessful == test.call().expectations.failure) || (output != test.call().expectations.rawBytes()))
					success = false;

				test.setFailure(!m_transactionSuccessful);
				test.setRawBytes(std::move(output));
				test.setContractABI(m_compiler.contractABI(m_compiler.lastContractName()));
			}
		}

		if (!success)
		{
			AnsiColorized(_stream, _formatted, {BOLD, CYAN}) << _linePrefix << "Expected result:" << endl;
			for (auto const& test: m_tests)
			{
				ErrorReporter errorReporter;
				_stream << test.format(errorReporter, _linePrefix, false, _formatted) << endl;
				_stream << errorReporter.format(_linePrefix, _formatted);
			}
			_stream << endl;
			AnsiColorized(_stream, _formatted, {BOLD, CYAN}) << _linePrefix << "Obtained result:" << endl;
			for (auto const& test: m_tests)
			{
				ErrorReporter errorReporter;
				_stream << test.format(errorReporter, _linePrefix, true, _formatted) << endl;
				_stream << errorReporter.format(_linePrefix, _formatted);
			}
			AnsiColorized(_stream, _formatted, {BOLD, RED}) << _linePrefix << endl << _linePrefix
				<< "Attention: Updates on the test will apply the detected format displayed." << endl;
			if (compileViaYul && m_runWithoutYul)
			{
				_stream << _linePrefix << endl << _linePrefix;
				AnsiColorized(_stream, _formatted, {RED_BACKGROUND}) << "Note that the test passed without Yul.";
				_stream << endl;
			}
			else if (!compileViaYul && m_runWithYul)
				AnsiColorized(_stream, _formatted, {BOLD, YELLOW}) << _linePrefix << endl << _linePrefix
					<< "Note that the test also has to pass via Yul." << endl;
			return TestResult::Failure;
		}
	}

	return TestResult::Success;
}

void SemanticTest::printSource(ostream& _stream, string const& _linePrefix, bool) const
{
	stringstream stream(m_source);
	string line;
	while (getline(stream, line))
		_stream << _linePrefix << line << endl;
}

void SemanticTest::printUpdatedExpectations(ostream& _stream, string const&) const
{
	for (auto const& test: m_tests)
		_stream << test.format("", true, false) << endl;
}

void SemanticTest::parseExpectations(istream& _stream)
{
	TestFileParser parser{_stream};
	auto functionCalls = parser.parseFunctionCalls(m_lineOffset);
	std::move(functionCalls.begin(), functionCalls.end(), back_inserter(m_tests));
}

bool SemanticTest::deploy(string const& _contractName, u256 const& _value, bytes const& _arguments, map<string, solidity::test::Address> const& _libraries)
{
	auto output = compileAndRunWithoutCheck(m_source, _value, _contractName, _arguments, _libraries);
	return !output.empty() && m_transactionSuccessful;
}
